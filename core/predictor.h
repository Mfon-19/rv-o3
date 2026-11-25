// Branch predictor combining a set-associative BTB, Return Address Stack (RAS)
// with optional squash repair, and a bimodal/gshare or TAGE direction predictor.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"
#include "sim/config.h"

class Predictor {
public:
  explicit Predictor(const SimConfig &config);

  // Speculative history and RAS state snapshotted per fetched instruction so
  // flushes can roll back wrong-path updates.
  struct Checkpoint {
    uint32_t history = 0;
    uint32_t rasCount = 0;
    uint32_t rasTop = 0;
  };

  struct Prediction {
    bool taken = false;
    uint32_t target = 0;
    uint32_t phtIndex = 0;
    Checkpoint before;
  };

  Prediction predict(uint32_t pc);

  void restore(const Checkpoint &checkpoint);
  void replay(uint32_t pc, const Instr &instr, bool taken);
  void update(uint32_t pc, const Instr &instr, bool taken, uint32_t target,
              uint32_t phtIndex, const Checkpoint &before);

private:
  enum Kind : uint8_t { COND, UNCOND, CALL, RET };

  struct BtbEntry {
    bool valid = false;
    Kind kind = COND;
    uint32_t pc = 0;
    uint32_t target = 0;
    uint64_t lastUse = 0;
  };

  struct TageEntry {
    int8_t counter = 0;
    uint8_t useful = 0;
    uint16_t tag = 0;
  };

  struct TageLookup {
    int provider = -1;
    int alt = -1;
    bool providerPred = false;
    bool altPred = false;
    bool pred = false;
    bool weak = false;
  };

  static Kind kindOf(const Instr &instr);
  uint32_t phtIndexFor(uint32_t pc) const;
  BtbEntry *btbFind(uint32_t pc);
  void pushHistory(bool taken);

  const uint16_t *foldsAt(uint32_t pos) const {
    return &foldedHistory[(size_t)(pos & ringMask) * numTables * 3];
  }

  TageLookup tageLookup(uint32_t pc, uint32_t pos);
  void tageUpdate(uint32_t pc, bool taken, uint32_t pos);

  const bool rasRepair;
  std::vector<uint8_t> pht;
  uint32_t ghr = 0;
  uint32_t ghrMask = 0;

  std::vector<BtbEntry> btb;
  uint32_t btbWays = 0;
  uint32_t btbSets = 0;
  uint64_t btbClock = 0;

  std::vector<uint32_t> ras;
  uint32_t rasCount = 0;

  // TAGE state: geometric history lengths, tagged tables, circular bit history,
  // and pre-folded history snapshots indexed by `historyPos & ringMask`.
  const uint32_t numTables;
  const uint32_t tableBits;
  const uint32_t tagBits;

  std::vector<uint32_t> historyLengths;
  std::vector<std::vector<TageEntry>> tables;
  std::vector<uint8_t> historyBits;
  std::vector<uint16_t> foldedHistory;

  uint32_t historyPos = 0;
  uint32_t ringMask = 0;
  int8_t useAltOnWeak = 0;
  uint32_t updateCount = 0;
  uint32_t lfsr = 0xace1u;

  std::vector<uint32_t> tableIndices;
  std::vector<uint32_t> tableTags;
};
