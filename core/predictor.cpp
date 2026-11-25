// Branch predictor implementation: BTB lookup/replacement, RAS push/pop/repair,
// bimodal/gshare PHT, and geometric-history TAGE with folded history rings.

#include "core/predictor.h"

#include <bit>
#include <cmath>

// Updates a cyclic XOR-folded history register when shifting `bitIn` in and
// evicting `bitOut` using a precomputed `length % width` shift amount.
static inline uint16_t foldHistory(uint16_t compressed, uint32_t bitIn,
                                   uint32_t bitOut, uint32_t evictShift,
                                   uint32_t width, uint32_t mask) {
  uint32_t value = ((uint32_t)compressed << 1) ^ bitIn;
  value ^= bitOut << evictShift;
  value ^= value >> width;
  return (uint16_t)(value & mask);
}

template <class T>
static inline void updateSaturatingCounter(T &counter, bool increment, int minVal,
                                           int maxVal) {
  if (increment && counter < maxVal)
    counter++;
  else if (!increment && counter > minVal)
    counter--;
}

Predictor::Predictor(const SimConfig &config)
    : rasRepair(config.rasRepair),
      pht(1u << config.phtBits, 1),
      phtMask((1u << config.phtBits) - 1),
      ghrMask(config.ghrBits ? (1u << config.ghrBits) - 1 : 0),
      btb(config.btbEntries),
      btbWays(config.btbWays),
      btbSets(config.btbEntries / config.btbWays),
      btbSetMask(btbSets - 1),
      btbPow2(std::has_single_bit(btbSets)),
      ras(config.rasEntries),
      rasMask(config.rasEntries - 1),
      rasPow2(std::has_single_bit(config.rasEntries)),
      numTables(config.tageTables),
      tableBits(config.tageTableBits),
      tagBits(config.tageTagBits),
      tableMask((1u << config.tageTableBits) - 1),
      tagMask((1u << config.tageTagBits) - 1),
      tagMaskAlt((1u << (config.tageTagBits - 1)) - 1) {
  if (!numTables)
    return;

  // Build geometric history lengths from tageMinHist up to tageMaxHist and
  // precompute modulo shift amounts to avoid divisions on hot paths.
  for (uint32_t i = 0; i < numTables; i++) {
    const double fraction = numTables > 1 ? double(i) / (numTables - 1) : 1.0;
    const double ratio = double(config.tageMaxHist) / config.tageMinHist;
    const uint32_t histLen =
        (uint32_t)std::lround(config.tageMinHist * std::pow(ratio, fraction));

    historyLengths.push_back(histLen);
    histModTable.push_back((uint8_t)(histLen % tableBits));
    histModTag1.push_back((uint8_t)(histLen % tagBits));
    histModTag2.push_back((uint8_t)(histLen % (tagBits - 1)));
    pcShift.push_back((uint8_t)(tableBits - (i % tableBits)));
  }

  tageTables.assign((size_t)numTables << tableBits, TageEntry{});

  // Size the history ring so in-flight checkpoints never overwrite live entries.
  uint32_t ringSize = 64;
  const uint32_t minRing =
      config.tageMaxHist + 2 * (config.robSize + config.fetchQSize) + 64;
  while (ringSize < minRing)
    ringSize <<= 1;

  ringMask = ringSize - 1;
  historyBits.assign(ringSize, 0);
  foldedHistory.assign((size_t)ringSize * numTables * 3, 0);
  tableIndices.resize(numTables);
  tableTags.resize(numTables);
}

Predictor::Kind Predictor::kindOf(const Instr &instr) {
  if (isBranch(instr.op))
    return COND;
  if (instr.rd == 1)
    return CALL;
  if (instr.op == Op::JALR && instr.rd == 0 && instr.rs1 == 1)
    return RET;
  return UNCOND;
}

uint32_t Predictor::phtIndexFor(uint32_t pc) const {
  return ((pc >> 2) ^ (numTables ? 0 : ghr)) & phtMask;
}

Predictor::BtbEntry *Predictor::btbFind(uint32_t pc) {
  const uint32_t setIdx = btbSetIndex(pc);

  if (btbWays == 1) {
    BtbEntry &entry = btb[setIdx];
    return (entry.valid && entry.pc == pc) ? &entry : nullptr;
  }

  BtbEntry *set = &btb[(size_t)setIdx * btbWays];

  for (uint32_t way = 0; way < btbWays; way++) {
    if (set[way].valid && set[way].pc == pc) {
      set[way].lastUse = ++btbClock;
      return &set[way];
    }
  }

  return nullptr;
}

void Predictor::pushHistory(bool taken) {
  if (!numTables) {
    ghr = ((ghr << 1) | (taken ? 1u : 0u)) & ghrMask;
    return;
  }

  const uint16_t *prevFolds = foldsAt(historyPos);
  historyPos++;
  historyBits[historyPos & ringMask] = taken;

  uint16_t *nextFolds =
      &foldedHistory[(size_t)(historyPos & ringMask) * numTables * 3];

  for (uint32_t i = 0; i < numTables; i++) {
    const uint32_t bitOut = historyBits[(historyPos - historyLengths[i]) & ringMask];

    nextFolds[3 * i] = foldHistory(prevFolds[3 * i], taken, bitOut,
                                   histModTable[i], tableBits, tableMask);
    nextFolds[3 * i + 1] = foldHistory(prevFolds[3 * i + 1], taken, bitOut,
                                       histModTag1[i], tagBits, tagMask);
    nextFolds[3 * i + 2] = foldHistory(prevFolds[3 * i + 2], taken, bitOut,
                                       histModTag2[i], tagBits - 1, tagMaskAlt);
  }
}

Predictor::TageLookup Predictor::tageLookup(uint32_t pc, uint32_t pos) {
  const uint16_t *folds = foldsAt(pos);
  const uint32_t wordPc = pc >> 2;

  for (uint32_t i = 0; i < numTables; i++) {
    tableIndices[i] = (wordPc ^ (wordPc >> pcShift[i]) ^ folds[3 * i]) & tableMask;
    tableTags[i] =
        (wordPc ^ folds[3 * i + 1] ^ ((uint32_t)folds[3 * i + 2] << 1)) & tagMask;
  }

  TageLookup lookup;

  // Select the longest-history matching table as provider and the next longest as alt.
  for (int i = (int)numTables - 1; i >= 0; i--) {
    if (tageEntry((uint32_t)i, tableIndices[i]).tag == tableTags[i]) {
      if (lookup.provider < 0) {
        lookup.provider = i;
      } else {
        lookup.alt = i;
        break;
      }
    }
  }

  const bool basePred = pht[wordPc & phtMask] >= 2;
  lookup.altPred =
      lookup.alt >= 0
          ? tageEntry((uint32_t)lookup.alt, tableIndices[lookup.alt]).counter >= 0
          : basePred;

  if (lookup.provider < 0) {
    lookup.pred = basePred;
    lookup.providerPred = basePred;
    return lookup;
  }

  const int8_t counter =
      tageEntry((uint32_t)lookup.provider, tableIndices[lookup.provider]).counter;
  lookup.providerPred = counter >= 0;
  lookup.weak = counter == 0 || counter == -1;
  lookup.pred = lookup.weak && useAltOnWeak >= 0 ? lookup.altPred
                                                 : lookup.providerPred;

  return lookup;
}

void Predictor::tageUpdate(uint32_t pc, bool taken, uint32_t pos) {
  const TageLookup lookup = tageLookup(pc, pos);
  uint8_t &baseCounter = pht[(pc >> 2) & phtMask];

  if (lookup.provider >= 0) {
    TageEntry &entry = tageEntry((uint32_t)lookup.provider, tableIndices[lookup.provider]);

    if (lookup.weak && lookup.providerPred != lookup.altPred)
      updateSaturatingCounter(useAltOnWeak, lookup.altPred == taken, -8, 7);

    if (lookup.providerPred != lookup.altPred)
      updateSaturatingCounter(entry.useful, lookup.providerPred == taken, 0, 3);

    updateSaturatingCounter(entry.counter, taken, -4, 3);

    if (lookup.alt < 0 && lookup.weak)
      updateSaturatingCounter(baseCounter, taken, 0, 3);
  } else {
    updateSaturatingCounter(baseCounter, taken, 0, 3);
  }

  // On misprediction, allocate into a longer-history table with useful == 0,
  // or age down useful counters if none is free.
  if (lookup.pred != taken && lookup.provider < (int)numTables - 1) {
    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xb400u);

    int firstFree = -1;
    int secondFree = -1;

    for (uint32_t t = (uint32_t)(lookup.provider + 1); t < numTables; t++) {
      if (tageEntry(t, tableIndices[t]).useful == 0) {
        if (firstFree < 0) {
          firstFree = (int)t;
        } else {
          secondFree = (int)t;
          break;
        }
      }
    }

    if (firstFree < 0) {
      for (uint32_t t = (uint32_t)(lookup.provider + 1); t < numTables; t++) {
        TageEntry &entry = tageEntry(t, tableIndices[t]);
        if (entry.useful > 0)
          entry.useful--;
      }
    } else {
      const uint32_t allocTable =
          (uint32_t)(secondFree >= 0 && (lfsr & 3) == 0 ? secondFree : firstFree);
      TageEntry &entry = tageEntry(allocTable, tableIndices[allocTable]);
      entry.tag = (uint16_t)tableTags[allocTable];
      entry.counter = taken ? 0 : -1;
      entry.useful = 0;
    }
  }

  // Periodically halve useful counters so stale entries can be reclaimed.
  if ((++updateCount & ((1u << 18) - 1)) == 0) {
    for (TageEntry &entry : tageTables)
      entry.useful >>= 1;
  }
}

Predictor::Prediction Predictor::predict(uint32_t pc) {
  Prediction prediction;
  prediction.before = checkpoint();
  prediction.phtIndex = phtIndexFor(pc);

  const BtbEntry *entry = btbFind(pc);
  if (!entry)
    return prediction;

  switch (entry->kind) {
  case COND:
    prediction.taken = numTables ? tageLookup(pc, historyPos).pred
                                 : pht[prediction.phtIndex] >= 2;
    prediction.target = entry->target;
    pushHistory(prediction.taken);
    break;

  case CALL:
    ras[rasSlot(rasCount)] = pc + 4;
    rasCount++;
    rasTopValue = pc + 4;
    prediction.taken = true;
    prediction.target = entry->target;
    break;

  case RET:
    prediction.taken = true;
    if (rasCount > 0) {
      rasCount--;
      prediction.target = ras[rasSlot(rasCount)];
      rasTopValue = rasCount ? ras[rasSlot(rasCount - 1)] : 0;
    } else {
      prediction.target = entry->target;
    }
    break;

  case UNCOND:
    prediction.taken = true;
    prediction.target = entry->target;
    break;
  }

  return prediction;
}

void Predictor::restore(const Checkpoint &saved) {
  if (numTables)
    historyPos = saved.history;
  else
    ghr = saved.history;

  if (rasRepair) {
    rasCount = saved.rasCount;
    if (rasCount) {
      ras[rasSlot(rasCount - 1)] = saved.rasTop;
      rasTopValue = saved.rasTop;
    } else {
      rasTopValue = 0;
    }
  }
}

// Re-applies the resolved branch outcome after rolling back speculative state.
void Predictor::replay(uint32_t pc, const Instr &instr, bool taken) {
  const Kind kind = kindOf(instr);

  if (kind == COND) {
    pushHistory(taken);
  } else if (rasRepair && kind == CALL) {
    ras[rasSlot(rasCount)] = pc + 4;
    rasCount++;
    rasTopValue = pc + 4;
  } else if (rasRepair && kind == RET && rasCount > 0) {
    rasCount--;
    rasTopValue = rasCount ? ras[rasSlot(rasCount - 1)] : 0;
  }
}

void Predictor::update(uint32_t pc, const Instr &instr, bool taken,
                       uint32_t target, uint32_t phtIndex,
                       const Checkpoint &before) {
  const Kind kind = kindOf(instr);

  if (kind == COND) {
    if (numTables) {
      tageUpdate(pc, taken, before.history);
    } else {
      uint8_t &counter = pht[phtIndex];
      if (taken && counter < 3)
        counter++;
      else if (!taken && counter > 0)
        counter--;
    }
  }

  if (taken) {
    BtbEntry *entry = btbFind(pc);

    if (!entry) {
      if (btbWays == 1) {
        entry = &btb[btbSetIndex(pc)];
      } else {
        BtbEntry *set = &btb[(size_t)btbSetIndex(pc) * btbWays];
        entry = &set[0];

        for (uint32_t way = 0; way < btbWays; way++) {
          if (!set[way].valid) {
            entry = &set[way];
            break;
          }
          if (set[way].lastUse < entry->lastUse)
            entry = &set[way];
        }
      }
    }

    entry->valid = true;
    entry->pc = pc;
    entry->target = target;
    entry->kind = kind;
    if (btbWays > 1)
      entry->lastUse = ++btbClock;
  }
}
