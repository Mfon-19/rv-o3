// Branch predictor implementation: BTB lookup/replacement, RAS push/pop/repair,
// bimodal/gshare PHT, and geometric-history TAGE with folded history rings.

#include "core/predictor.h"

#include <cmath>

// Updates a cyclic XOR-folded history register when shifting `bitIn` in and
// evicting `bitOut` after `length` bits.
static inline uint16_t foldHistory(uint16_t compressed, uint32_t bitIn,
                                   uint32_t bitOut, uint32_t length,
                                   uint32_t width) {
  uint32_t value = ((uint32_t)compressed << 1) ^ bitIn;
  value ^= bitOut << (length % width);
  value ^= value >> width;
  return (uint16_t)(value & ((1u << width) - 1));
}

template <class T>
static void updateSaturatingCounter(T &counter, bool increment, int minVal,
                                    int maxVal) {
  if (increment && counter < maxVal)
    counter++;
  else if (!increment && counter > minVal)
    counter--;
}

Predictor::Predictor(const SimConfig &config)
    : rasRepair(config.rasRepair),
      pht(1u << config.phtBits, 1),
      ghrMask(config.ghrBits ? (1u << config.ghrBits) - 1 : 0),
      btb(config.btbEntries),
      btbWays(config.btbWays),
      btbSets(config.btbEntries / config.btbWays),
      ras(config.rasEntries),
      numTables(config.tageTables),
      tableBits(config.tageTableBits),
      tagBits(config.tageTagBits) {
  if (!numTables)
    return;

  // Build geometric history lengths from tageMinHist up to tageMaxHist.
  for (uint32_t i = 0; i < numTables; i++) {
    const double fraction = numTables > 1 ? double(i) / (numTables - 1) : 1.0;
    const double ratio = double(config.tageMaxHist) / config.tageMinHist;
    historyLengths.push_back(
        (uint32_t)std::lround(config.tageMinHist * std::pow(ratio, fraction)));
  }

  tables.assign(numTables, std::vector<TageEntry>(1u << tableBits));

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
  return ((pc >> 2) ^ (numTables ? 0 : ghr)) & (uint32_t)(pht.size() - 1);
}

Predictor::BtbEntry *Predictor::btbFind(uint32_t pc) {
  BtbEntry *set = &btb[(size_t)((pc >> 2) % btbSets) * btbWays];

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
    ghr = ((ghr << 1) | (taken ? 1 : 0)) & ghrMask;
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
                                   historyLengths[i], tableBits);
    nextFolds[3 * i + 1] = foldHistory(prevFolds[3 * i + 1], taken, bitOut,
                                       historyLengths[i], tagBits);
    nextFolds[3 * i + 2] = foldHistory(prevFolds[3 * i + 2], taken, bitOut,
                                       historyLengths[i], tagBits - 1);
  }
}

Predictor::TageLookup Predictor::tageLookup(uint32_t pc, uint32_t pos) {
  const uint16_t *folds = foldsAt(pos);
  const uint32_t wordPc = pc >> 2;
  const uint32_t idxMask = (1u << tableBits) - 1;
  const uint32_t tagMask = (1u << tagBits) - 1;

  for (uint32_t i = 0; i < numTables; i++) {
    tableIndices[i] =
        (wordPc ^ (wordPc >> (tableBits - i % tableBits)) ^ folds[3 * i]) &
        idxMask;
    tableTags[i] =
        (wordPc ^ folds[3 * i + 1] ^ ((uint32_t)folds[3 * i + 2] << 1)) &
        tagMask;
  }

  TageLookup lookup;

  // Select the longest-history matching table as provider and the next longest as alt.
  for (int i = (int)numTables - 1; i >= 0; i--) {
    if (tables[i][tableIndices[i]].tag == tableTags[i]) {
      if (lookup.provider < 0) {
        lookup.provider = i;
      } else {
        lookup.alt = i;
        break;
      }
    }
  }

  const bool basePred = pht[wordPc & (pht.size() - 1)] >= 2;
  lookup.altPred = lookup.alt >= 0
                       ? tables[lookup.alt][tableIndices[lookup.alt]].counter >= 0
                       : basePred;

  if (lookup.provider < 0) {
    lookup.pred = basePred;
    lookup.providerPred = basePred;
    return lookup;
  }

  const int8_t counter =
      tables[lookup.provider][tableIndices[lookup.provider]].counter;
  lookup.providerPred = counter >= 0;
  lookup.weak = counter == 0 || counter == -1;
  lookup.pred = lookup.weak && useAltOnWeak >= 0 ? lookup.altPred
                                                 : lookup.providerPred;

  return lookup;
}

void Predictor::tageUpdate(uint32_t pc, bool taken, uint32_t pos) {
  const TageLookup lookup = tageLookup(pc, pos);
  uint8_t &baseCounter = pht[(pc >> 2) & (pht.size() - 1)];

  if (lookup.provider >= 0) {
    TageEntry &entry = tables[lookup.provider][tableIndices[lookup.provider]];

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
      if (tables[t][tableIndices[t]].useful == 0) {
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
        if (tables[t][tableIndices[t]].useful > 0)
          tables[t][tableIndices[t]].useful--;
      }
    } else {
      const int allocTable =
          secondFree >= 0 && (lfsr & 3) == 0 ? secondFree : firstFree;
      TageEntry &entry = tables[allocTable][tableIndices[allocTable]];
      entry.tag = (uint16_t)tableTags[allocTable];
      entry.counter = taken ? 0 : -1;
      entry.useful = 0;
    }
  }

  // Periodically halve useful counters so stale entries can be reclaimed.
  if ((++updateCount & ((1u << 18) - 1)) == 0) {
    for (auto &table : tables) {
      for (TageEntry &entry : table)
        entry.useful >>= 1;
    }
  }
}

Predictor::Prediction Predictor::predict(uint32_t pc) {
  Prediction prediction;
  prediction.before.history = numTables ? historyPos : ghr;
  prediction.before.rasCount = rasCount;
  prediction.before.rasTop = rasCount ? ras[(rasCount - 1) % ras.size()] : 0;
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
    ras[rasCount % ras.size()] = pc + 4;
    rasCount++;
    prediction.taken = true;
    prediction.target = entry->target;
    break;

  case RET:
    prediction.taken = true;
    if (rasCount > 0) {
      rasCount--;
      prediction.target = ras[rasCount % ras.size()];
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

void Predictor::restore(const Checkpoint &checkpoint) {
  if (numTables)
    historyPos = checkpoint.history;
  else
    ghr = checkpoint.history;

  if (rasRepair) {
    rasCount = checkpoint.rasCount;
    if (rasCount)
      ras[(rasCount - 1) % ras.size()] = checkpoint.rasTop;
  }
}

// Re-applies the resolved branch outcome after rolling back speculative state.
void Predictor::replay(uint32_t pc, const Instr &instr, bool taken) {
  const Kind kind = kindOf(instr);

  if (kind == COND) {
    pushHistory(taken);
  } else if (rasRepair && kind == CALL) {
    ras[rasCount % ras.size()] = pc + 4;
    rasCount++;
  } else if (rasRepair && kind == RET && rasCount > 0) {
    rasCount--;
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
      BtbEntry *set = &btb[(size_t)((pc >> 2) % btbSets) * btbWays];
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

    entry->valid = true;
    entry->pc = pc;
    entry->target = target;
    entry->kind = kind;
    entry->lastUse = ++btbClock;
  }
}
