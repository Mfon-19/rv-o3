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
//      - Fetch: an aligned 8-byte pair per cycle from the L1I port,
//        steered by the branch predictor (gshare + BTB + RAS), into a
//        small fetch queue
//      - Decode/rename (2/cycle, in order): architectural sources map
//        to physical registers; each destination gets a fresh physical
//        register from the free list. WAW and WAR hazards cease to
//        exist here; only true RAW dependencies remain
//      - Dispatch allocates a ROB entry (and an LSQ entry for memory
//        ops) in program order and drops the instruction into the
//        issue queue
//      - Issue (2/cycle, OUT of order): oldest-ready-first among
//        entries whose operands and functional unit are available.
//        Values are read from the physical register file at issue;
//        execute() computes results immediately and the units delay
//        their visibility
//      - Writeback (2 ports, oldest-first arbitration): writes the
//        physical register, broadcasts the register number to wake the
//        issue queue, marks the ROB entry done. Branches resolve here:
//        a mispredict flushes everything younger and restores the
//        rename map by walking the ROB tail, reclaiming physical
//        registers
//      - Commit (2/cycle, in order): the only place architectural
//        state changes. Displaced mappings are freed, stores are
//        released to the store buffer, syscalls/traps take effect,
//        the predictor trains. Faults detected on the wrong path are
//        carried to commit and only then become fatal — the machine
//        is precise at every instruction boundary
//      - Memory (conservative, core/lsq.h): loads wait for all older
//        store addresses; exact-match stores forward; stores write the
//        cache only after commit via the store buffer
//      - System ops serialize: they dispatch only into an empty
//        machine (ROB and store buffer drained) and act at commit
//
// Simulation technique
//      Stages run in reverse pipeline order each cycle (commit,
//      writeback, LSQ, issue, dispatch, fetch), so each consumes what
//      the stage behind it produced in an earlier cycle; units tick
//      at the clock edge, and
//      writeback-before-issue makes a value written back in cycle N
//      issueable in cycle N — the wakeup path needs no forwarding
//      network.

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
  // map — the differential checker compares final state with this
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
  uint32_t pc = 0; // next address fetch will request
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
