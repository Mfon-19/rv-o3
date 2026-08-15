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
//        their visibility, exactly as in the in-order core
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
//      Same discipline as the in-order core: stages run in reverse
//      pipeline order each cycle (commit, writeback, LSQ, issue,
//      dispatch, fetch), units tick at the clock edge, and
//      writeback-before-issue makes a value written back in cycle N
//      issueable in cycle N — the wakeup path needs no forwarding
//      network.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "core/core.h"
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

class OoOCore : public Core {
public:
  OoOCore(const SimConfig &cfg, MemorySystem &msys);

  void loadWords(const std::vector<uint32_t> &words) override;
  void loadBytes(const std::vector<uint8_t> &bytes) override;
  int run() override;
  void dumpRegs() const override;
  // Architectural registers live behind the rename map
  uint32_t reg(int i) const override { return prf.val[rmap.map[i]]; }

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

  // functional units (shared FuUnit model with the in-order core)
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

  // data-port ownership (one blocking port, three kinds of customer)
  enum class DPort { FREE, LOAD, LOAD_STALE, STORE };
  DPort dport = DPort::FREE;
  uint32_t dLoadLsqIdx = 0;
  uint64_t dLoadSeq = 0;

  FuOp loadOut; // completed load waiting for a writeback port

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
