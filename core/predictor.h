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

  // O(1) snapshot for non-control-flow instructions that skip BTB/PHT lookup.
  Checkpoint checkpoint() const {
    return Checkpoint{numTables ? historyPos : ghr, rasCount, rasTopValue};
  }

  Prediction predict(uint32_t pc);

  void restore(const Checkpoint &checkpoint);
  void replay(uint32_t pc, const Instr &instr, bool taken);
  void update(uint32_t pc, const Instr &instr, bool taken, uint32_t target,
              uint32_t phtIndex, const Checkpoint &before);

private:
  enum Kind : uint8_t { COND, UNCOND, CALL, RET };

  struct BtbEntry {
    uint32_t pc = 0;
    uint32_t target = 0;
    uint32_t lastUse = 0;
    Kind kind = COND;
    bool valid = false;
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
  uint32_t btbSetIndex(uint32_t pc) const {
    const uint32_t wordPc = pc >> 2;
    return btbPow2 ? (wordPc & btbSetMask) : (wordPc % btbSets);
  }
  uint32_t rasSlot(uint32_t index) const {
    return rasPow2 ? (index & rasMask) : (index % (uint32_t)ras.size());
  }

  BtbEntry *btbFind(uint32_t pc);
  void pushHistory(bool taken);

  const uint16_t *foldsAt(uint32_t pos) const {
    return &foldedHistory[(size_t)(pos & ringMask) * numTables * 3];
  }

  TageEntry &tageEntry(uint32_t tableIdx, uint32_t entryIdx) {
    return tageTables[((size_t)tableIdx << tableBits) | entryIdx];
  }

  TageLookup tageLookup(uint32_t pc, uint32_t pos);
  void tageUpdate(uint32_t pc, bool taken, uint32_t pos);

  const bool rasRepair;
  std::vector<uint8_t> pht;
  uint32_t phtMask = 0;
  uint32_t ghr = 0;
  uint32_t ghrMask = 0;

  std::vector<BtbEntry> btb;
  uint32_t btbWays = 0;
  uint32_t btbSets = 0;
  uint32_t btbSetMask = 0;
  bool btbPow2 = false;
  uint32_t btbClock = 0;

  std::vector<uint32_t> ras;
  uint32_t rasMask = 0;
  bool rasPow2 = false;
  uint32_t rasCount = 0;
  uint32_t rasTopValue = 0;

  // TAGE state: geometric history lengths, flat tagged tables, circular bit
  // history, and pre-folded history snapshots indexed by `historyPos & ringMask`.
  const uint32_t numTables;
  const uint32_t tableBits;
  const uint32_t tagBits;
  uint32_t tableMask = 0;
  uint32_t tagMask = 0;
  uint32_t tagMaskAlt = 0;

  std::vector<uint32_t> historyLengths;
  std::vector<uint8_t> histModTable;
  std::vector<uint8_t> histModTag1;
  std::vector<uint8_t> histModTag2;
  std::vector<uint8_t> pcShift;
  std::vector<TageEntry> tageTables;
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
