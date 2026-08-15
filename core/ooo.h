// ==============================================================
// The out-of-order superscalar core
// ==============================================================
//
//            +-------+  +--------+  +----------+   +-----+  +----+
//   2/cycle  | fetch |->| decode |->| dispatch |-->| IQ  |->| FU |--+
//   -------> | +pred |  | rename |  | ROB/LSQ  |   |     |  |    |  |
//            +-------+  +--------+  +----------+   +-----+  +----+  |
//                                        ROB <-- writeback/wakeup <-+
//                                         |
//                                      commit (2/cycle, in order)
//
// Microarchitecture
//      - Fetch reads an aligned 8-byte pair per cycle from the L1I
//        port into a small queue. The branch predictor (a direction
//        table, a branch target buffer, and a return-address stack)
//        decides each cycle where fetch continues, so taken branches
//        cost nothing when predicted correctly
//      - Decode/rename (2/cycle, in order): each source register is
//        mapped to the physical register currently holding its value,
//        and each destination gets a fresh physical register from the
//        free list. Because no physical register is ever written
//        twice, two writers of the same architectural register can no
//        longer conflict; the only waiting left in the machine is for
//        values that genuinely do not exist yet
//      - Dispatch allocates a ROB entry (and an LSQ entry for memory
//        ops) in program order and drops the instruction into the
//        issue queue
//      - Issue (2/cycle, OUT of order): each cycle the oldest
//        instructions whose operands are ready and whose functional
//        unit is free leave the queue, regardless of program order.
//        Operand values are read from the physical register file at
//        issue; execute() computes the result immediately and the
//        unit only delays when that result becomes visible
//      - Writeback (2 ports; when more results finish than ports
//        exist, the oldest go first): writes the physical register,
//        announces the register number so waiting instructions in the
//        issue queue become ready, and marks the ROB entry done. A
//        branch is checked against its prediction here, the moment
//        the branch unit produces the real outcome; on a wrong
//        prediction everything younger is flushed, the rename map is
//        restored by walking the ROB from youngest to oldest, and the
//        squashed physical registers are returned
//      - Commit (2/cycle, in order): the only place architectural
//        state changes. Displaced mappings are freed, stores are
//        released to the store buffer, syscalls take effect, the
//        predictor trains. A fault detected on a speculative path is
//        only carried along, and becomes fatal only if its
//        instruction commits; stopped at any commit boundary, the
//        machine shows a state that a plain one-instruction-at-a-time
//        execution could have produced
//      - Memory ordering is configurable (core/lsq.h). By default
//        loads may run ahead of older stores whose addresses are not
//        known yet; if such a store later turns out to overlap, the
//        load re-executes (a replay). Stores write the cache only
//        after commit, through the store buffer
//      - System ops (ecall, ebreak, fence) wait until the whole
//        machine has emptied, run alone, and take effect at commit,
//        so they always observe fully settled state
//
// Simulation technique
//      Stages run in reverse pipeline order each cycle (commit,
//      writeback, LSQ, issue, dispatch, fetch), so each stage
//      consumes what the stage behind it produced in an earlier
//      cycle; the functional units advance at the end of the cycle,
//      like a clock edge. One ordering is deliberate: writeback runs
//      before issue, so a value written back in cycle N can already
//      be read by an instruction issuing in cycle N. That single rule
//      plays the role a forwarding network plays in hardware.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "core/commit.h"
#include "core/fu.h"
#include "core/iq.h"
#include "core/lsq.h"
#include "core/predictor.h"
#include "core/rename.h"
#include "core/rob.h"
#include "isa/isa.h"
#include "memory/system.h"
#include "sim/config.h"
#include "sim/stats.h"

class OoOCore {
public:
  OoOCore(const SimConfig &cfg, MemorySystem &msys);

  // Load a program image at address 0
  void loadWords(const std::vector<uint32_t> &words);
  void loadBytes(const std::vector<uint8_t> &bytes);

  // Run until an exit syscall / ebreak / error, or the cycle budget
  // runs out; returns the exit code
  int run();

  void dumpRegs() const;

  // The architectural value of register i, read through the rename
  // map; the differential checker compares final state with this
  uint32_t reg(int i) const { return prf.val[rmap.map[i]]; }

  // Called once per committed instruction with its CommitRecord, in
  // commit order. Null by default; the differential checker plugs in
  // here to compare the core against the functional reference model
  std::function<void(const CommitRecord &)> onCommit;

private:
  const SimConfig cfg;
  MemorySystem &msys;
  MemPort *imem;
  MemPort *dmem;
  bool showMemStats;

  // rename state + physical register file
  RenameMap rmap;
  FreeList freeList;
  PhysRegFile prf;

  // machine structures
  ROB rob;
  IssueQueue iq;
  LSQ lsq;
  StoreBuffer sb;
  Predictor pred;

  // functional units
  std::vector<FuUnit> alus;
  FuUnit brUnit, mulUnit, divUnit;
  FuUnit agu; // address generation; drains to the LSQ, not a WB port

  // fetch state
  uint32_t pc = 0;     // next address fetch will request
  uint32_t fBytes = 8; // aligned fetch-block size: max(8, width*4)
  struct Fetched {
    uint32_t pc = 0, raw = 0;
    bool predTaken = false;
    uint32_t predTarget = 0;
    uint32_t predIdx = 0, ghrBefore = 0; // predictor snapshot
  };
  std::deque<Fetched> fetchQ;
  bool fOutstanding = false, fStale = false;
  uint32_t fBlock = 0, fFetchPc = 0;

  // several loads and committed stores can be in flight at the L1D at
  // once; completions match back through {lsq slot, generation} tags
  // for loads and store-buffer transaction ids for stores
  std::vector<FuOp> loadOuts; // completed loads awaiting WB ports
  uint32_t sbTxnCtr = 0;

  // memory-dependence predictor: loads that violated recently are
  // issued conservatively (Speculative mode only)
  std::vector<uint8_t> depTable;

  // control
  uint64_t seqCtr = 0;
  bool pendRedirect = false;
  uint32_t pendTarget = 0;
  bool recTimerArmed = false; // attributes cycles lost to recovery:
  uint64_t recTimerStart = 0; // recover() to the next real dispatch
  bool halted = false;
  int exitCode = 0;
  bool trace;
  Stats stats;
  std::vector<std::string> events;

  // stages, in the order stepCycle runs them
  void commitStage();
  void writebackStage();
  void recover(uint64_t seq); // flush younger than seq everywhere
  void aguDrain();
  void violationScan(const LsqEntry &store); // replay loads that jumped
  void drainDataResponses();
  void lsqOperate();
  void issueStage();
  void dispatchStage();
  void fetchStage();
  void processFetch(const MemResponse &r);
  void tickUnits();
  void doSyscall(const RobEntry &e);
  void stepCycle();

  // trace
  std::string vOcc, vDS, vIS, vWB, vCT;
  void captureView();
  void printTrace() const;
  void note(std::string &v, const Instr &ins);
};
