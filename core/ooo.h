// Superscalar out-of-order RV32IM processor core: register renaming, physical
// register file, ROB, issue queue, LSQ, store buffer, functional units, and
// branch prediction with precise mispredict and load-replay recovery.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/fu.h"
#include "core/iq.h"
#include "core/lsq.h"
#include "core/predictor.h"
#include "core/refmodel.h"
#include "core/ring.h"
#include "isa/isa.h"
#include "memory/system.h"
#include "sim/config.h"

struct PhysRegFile {
  std::vector<uint32_t> values;
  std::vector<uint8_t> ready;

  explicit PhysRegFile(uint32_t numRegs) : values(numRegs, 0), ready(numRegs, 1) {}
};

using FreeList = Ring<uint8_t>;

// Maps the 32 architectural RISC-V registers to current physical register IDs.
struct RenameMap {
  uint8_t archToPhys[32];

  void reset() {
    for (int i = 0; i < 32; i++)
      archToPhys[i] = (uint8_t)i;
  }
};

struct RobEntry {
  uint64_t seq = 0;
  uint32_t pc = 0;
  Instr instr;
  bool done = false;

  uint8_t destPhysReg = 0xFF;
  uint8_t prevPhysReg = 0xFF; // Returned to free list on commit (or restored on squash).
  bool isMem = false;
  bool isSystem = false;
  uint8_t fault = 0; // 1 = misaligned, 2 = out-of-range; raised only at commit.

  bool isBranch = false;
  bool predictedTaken = false;
  bool actualTaken = false;
  uint32_t predictedTarget = 0;
  uint32_t actualTarget = 0;
  uint32_t phtIndex = 0;
  Predictor::Checkpoint predCheckpoint;

  uint8_t syscallArgPhysReg = 0;
  uint8_t syscallNumPhysReg = 0;

  bool mispredicted() const {
    return actualTaken != predictedTaken ||
           (actualTaken && actualTarget != predictedTarget);
  }
};

using ROB = Ring<RobEntry>;

struct CoreStats {
  uint64_t cycles = 0;
  uint64_t retired = 0;
  uint64_t branches = 0;
  uint64_t mispredicts = 0;
  uint64_t flushes = 0;
  uint64_t loadsForwarded = 0;
  uint64_t loadReplays = 0;
};

class OoOCore {
public:
  OoOCore(const SimConfig &config, MemorySystem &memSystem);

  int run();
  void dumpRegs() const;
  uint32_t reg(int idx) const { return physRegs.values[renameMap.archToPhys[idx]]; }

  // Optional callback invoked in program order for every committed instruction.
  std::function<void(const CommitRecord &)> onCommit;

private:
  struct FetchedInstr {
    uint32_t pc = 0;
    uint32_t raw = 0;
    bool predTaken = false;
    uint32_t predTarget = 0;
    uint32_t phtIndex = 0;
    Predictor::Checkpoint predCheckpoint;
  };

  void commitStage();
  void writebackStage();
  void recover(uint64_t keepSeq);
  void aguDrain();
  void violationScan(const LsqEntry &store);
  void drainDataResponses();
  void lsqOperate();
  FuUnit *acquireUnit(FuKind kind, size_t &nextAlu, size_t &nextAgu);
  void issueStage();
  void dispatchStage();
  void fetchStage();
  void processFetch(const MemResponse &response);
  void stepCycle();
  void printStats() const;

  const SimConfig config;
  MemorySystem &memSystem;
  MemPort *imem = nullptr;
  MemPort *dmem = nullptr;

  RenameMap renameMap;
  FreeList freeList;
  PhysRegFile physRegs;
  ROB rob;
  IssueQueue iq;
  LSQ lsq;
  StoreBuffer storeBuffer;
  Predictor predictor;

  std::vector<FuUnit> alus;
  FuUnit branchUnit;
  FuUnit mulUnit;
  FuUnit divUnit;
  std::vector<FuUnit> agus;
  std::vector<FuUnit *> execUnits;

  std::vector<FuOp *> wbReady;
  std::vector<FuOp> loadOutputs;
  uint32_t storeBufferTxnCounter = 0;

  uint32_t pc = 0;
  const uint32_t fetchBlockBytes;
  Ring<FetchedInstr> fetchQueue;
  bool fetchOutstanding = false;
  bool fetchStale = false; // Discards an in-flight I-fetch returned after a redirect.
  uint32_t fetchBlockAddr = 0;
  uint32_t fetchStartPc = 0;

  uint64_t nextSeq = 0;
  bool pendingRedirect = false;
  uint32_t pendingTarget = 0;
  bool halted = false;
  int exitCode = 0;
  CoreStats stats;
};
