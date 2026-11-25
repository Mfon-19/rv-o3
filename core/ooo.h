// Superscalar out-of-order RV32IM processor core: register renaming, physical
// register file, ROB, issue queue, LSQ, store buffer, functional units, and
// branch prediction with precise mispredict and load-replay recovery.

#pragma once

#include <array>
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
  static constexpr uint32_t kDecodeCacheSize = 4096;
  static constexpr uint32_t kMaxWbReady = 256;

  struct DecodedEntry {
    Instr instr;
    FuKind unit = FuKind::NONE;
    uint8_t flags = 0; // bit0=writesRd, bit1=usesRs1, bit2=usesRs2, bit3=isStore
    uint8_t memSize = 4;
  };

  struct FetchedInstr {
    uint32_t pc = 0;
    uint32_t raw = 0;
    bool predTaken = false;
    uint32_t predTarget = 0;
    uint32_t phtIndex = 0;
    Predictor::Checkpoint predCheckpoint;
  };

  const DecodedEntry &decodeCached(uint32_t instrPc, uint32_t raw);
  void commitStage();
  void writebackStage();
  void recover(uint64_t keepSeq);
  void aguDrain();
  void violationScan(const LsqEntry &store);
  void reportLoad(uint32_t lsqSlot, LsqEntry &entry);
  void drainDataResponses();
  void lsqOperate();
  [[gnu::always_inline]] bool tryIssueEntry(const IqEntry &entry,
                                            size_t &nextAlu, size_t &nextBranch);
  void issueStage();
  void dispatchStage();
  void fetchStage();
  void processFetch(const uint8_t *blockData);
  void stepCycle();
  void printStats() const;

  const SimConfig config;
  MemorySystem &memSystem;
  MemPort *imem = nullptr;
  MemPort *dmem = nullptr;
  Cache *l1iCache = nullptr;
  Cache *l1dCache = nullptr;
  const bool flatFastPath;

  RenameMap renameMap;
  FreeList freeList;
  PhysRegFile physRegs;
  ROB rob;
  IssueQueue iq;
  LSQ lsq;
  StoreBuffer storeBuffer;
  Predictor predictor;

  struct PendingAgu {
    uint64_t seq = 0;
    uint32_t addr = 0;
    uint16_t robIdx = 0;
    uint16_t lsqIdx = 0;
  };

  std::vector<FuUnit> alus;
  std::vector<FuUnit> branchUnits;
  FuUnit mulUnit;
  FuUnit divUnit;
  std::vector<FuUnit *> execUnits;
  uint32_t branchExecBegin = 0;
  uint32_t branchExecEnd = 0;
  uint32_t mulExecIdx = 0;
  uint32_t divExecIdx = 0;
  uint64_t activeExecMask = 0;

  std::array<PendingAgu, 16> pendingAgu{};
  uint32_t pendingAguCount = 0;

  std::array<FuOp *, kMaxWbReady> wbReady{};
  uint32_t wbReadyCount = 0;
  uint32_t wbBranchCount = 0;
  std::vector<FuOp> loadOutputs;
  uint32_t storeBufferTxnCounter = 0;

  // Incremental LSQ state counters used to skip unnecessary per-cycle scans.
  uint32_t lsqUnknownStores = 0;
  uint32_t lsqStoresWaitingData = 0;
  uint32_t lsqReadyLoads = 0;
  uint32_t lsqSpecIssuedLoads = 0;
  const bool lsqUseWaitMask;
  std::array<uint64_t, 256> storeDataWaitMask{};

  // Direct-mapped PC-indexed decode cache verified against raw word on hit.
  std::array<DecodedEntry, kDecodeCacheSize> decodeCache{};

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
