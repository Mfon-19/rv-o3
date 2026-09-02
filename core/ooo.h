// The out-of-order superscalar core.
//
//            +-------+  +--------+  +----------+   +-----+  +----+
//   2/cycle  | fetch |->| decode |->| dispatch |-->| IQ  |->| FU |--+
//   -------> | +pred |  | rename |  | ROB/LSQ  |   |     |  |    |  |
//            +-------+  +--------+  +----------+   +-----+  +----+  |
//                                        ROB <-- writeback/wakeup <-+
//                                         |
//                                      commit (2/cycle, in order)
//
// The widths shown are the shipped defaults; all of them are knobs.
//
// Microarchitecture
//      - Fetch pulls one aligned block per cycle (width*4 bytes, minimum
//        8) into a small queue. A direction table, a branch target
//        buffer, and a return-address stack steer the next block, so
//        correctly predicted taken branches cost nothing
//      - Rename (in order) maps each source register to the physical
//        register currently holding its value and hands each destination
//        a fresh one from the free list. No physical register is written
//        twice while live, so WAW and WAR name hazards cannot occur; the
//        only waiting left in the machine is for values that do not
//        exist yet
//      - Dispatch allocates a ROB entry (and an LSQ entry for memory
//        ops) in program order and drops the instruction into the
//        issue queue
//      - Issue (OUT of order) picks the oldest ready instructions whose
//        unit can accept them, regardless of program order. Operands
//        come from the physical register file; execute() computes the
//        result at issue and the unit only delays when it becomes
//        visible
//      - Writeback (wbPorts per cycle; when more results finish than
//        ports exist, the oldest go first) writes the physical register,
//        wakes the issue queue, and marks the ROB entry done. A branch is
//        checked against its prediction here, the moment the branch unit
//        produces the real outcome; on a wrong prediction everything
//        younger is flushed, the rename map is restored by walking the
//        ROB from youngest to oldest, and the squashed physical
//        registers are returned
//      - Commit (in order) is where state becomes architectural.
//        Displaced mappings are freed, stores move to the store buffer,
//        syscalls take effect, the predictor trains. A fault found on a
//        speculative path rides along and becomes fatal only if its
//        instruction commits, so stopped at any commit boundary the
//        machine shows a state that one-instruction-at-a-time execution
//        could have produced
//      - Memory ordering is a policy knob (core/lsq.h). By default loads
//        run ahead of older stores whose addresses are not known yet; a
//        store that later resolves to overlap replays the load. Stores
//        write the cache only after commit, through the store buffer
//      - System ops (ecall, ebreak, fence) dispatch only into an empty
//        machine, run alone, and take effect at commit
//
// Simulation technique
//      stepCycle runs the stages in reverse pipeline order (commit,
//      writeback, LSQ, issue, dispatch, fetch), so each stage consumes
//      what the stage behind it produced in an earlier cycle; the
//      functional units and the memory system advance at the end of the
//      cycle, like a clock edge. One ordering is deliberate: writeback
//      runs before issue, so a value written back in cycle N can already
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

  // Run until an exit syscall / ebreak / fatal error, or the cycle
  // budget runs out; returns the exit code
  int run();

  void dumpRegs() const;

  // The architectural value of register i, read through the rename map
  uint32_t reg(int i) const { return prf.val[rmap.map[i]]; }

  // Called once per committed instruction, in commit order. Null by
  // default; the differential checker plugs in here
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
  FuUnit agu;                  // address generation; drains to the LSQ
  std::vector<FuUnit *> units; // all of the above, for flush and tick

  // fetch state: at most one block request is outstanding at a time
  uint32_t pc = 0;      // next address fetch will request
  const uint32_t fBytes; // aligned fetch-block size: max(8, width*4)
  struct Fetched {
    uint32_t pc = 0, raw = 0;
    bool predTaken = false;
    uint32_t predTarget = 0;
    uint32_t predIdx = 0, ghrBefore = 0; // predictor snapshot
  };
  std::deque<Fetched> fetchQ;
  bool fOutstanding = false, fStale = false; // stale: squashed while in flight
  uint32_t fBlock = 0, fFetchPc = 0; // the outstanding block, and the pc in it

  // Several loads and committed stores can be in flight at the L1D at
  // once. Load completions match back through {lsq slot, generation}
  // tags, store acks through store-buffer transaction ids
  std::vector<FuOp> loadOuts; // completed loads awaiting WB ports
  uint32_t sbTxnCtr = 0;

  // Memory-dependence predictor (Speculative mode only): a load that
  // replayed recently waits for unknown store addresses instead
  std::vector<uint8_t> depTable;
  uint8_t &depEntry(uint32_t pc) {
    return depTable[(pc >> 2) % depTable.size()];
  }

  // control
  uint64_t seqCtr = 0;        // program-order sequence numbers (dispatch)
  bool pendRedirect = false;  // a redirect to apply at the clock edge
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
  void recover(uint64_t seq); // flush everything younger than seq
  void aguDrain();
  void violationScan(const LsqEntry &store); // replay loads that passed it
  void drainDataResponses();
  void lsqOperate();
  void issueStage();
  void dispatchStage();
  void fetchStage();
  void processFetch(const MemResponse &r);
  void stepCycle();

  // trace
  std::string vOcc, vDS, vIS, vWB, vCT;
  void captureView();
  void printTrace() const;
  void note(std::string &v, const Instr &ins);
};
