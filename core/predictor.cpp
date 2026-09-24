#include "core/predictor.h"

#include <cmath>

Predictor::Predictor(const SimConfig &cfg)
    : rasRepair(cfg.rasRepair),
      pht(1u << cfg.phtBits, 1), // weakly not-taken
      ghrMask(cfg.ghrBits ? (1u << cfg.ghrBits) - 1 : 0),
      btb(cfg.btbEntries), btbWays(cfg.btbWays),
      btbSets(cfg.btbEntries / cfg.btbWays), ras(cfg.rasEntries),
      nTables(cfg.tageTables), tableBits(cfg.tageTableBits),
      tagBits(cfg.tageTagBits) {
  if (!nTables)
    return;
  for (uint32_t i = 0; i < nTables; i++) {
    const double f = nTables > 1 ? double(i) / (nTables - 1) : 1.0;
    histLen.push_back((uint32_t)std::lround(
        cfg.tageMinHist * std::pow(double(cfg.tageMaxHist) / cfg.tageMinHist, f)));
  }
  tables.assign(nTables, std::vector<TageEntry>(1u << tableBits));
  // Every in-flight instruction pushes at most one bit, and a checkpoint
  // must still find its folds and the bits its window reads
  uint32_t ring = 64;
  while (ring < cfg.tageMaxHist + 2 * (cfg.robSize + cfg.fetchQSize) + 64)
    ring <<= 1;
  ringMask = ring - 1;
  histBits.assign(ring, 0);
  folded.assign((size_t)ring * nTables * 3, 0);
  idx.resize(nTables);
  tag.resize(nTables);
}

// The standard RISC-V idioms: jal/jalr with rd = ra is a call,
// jalr x0, 0(ra) is a return
Predictor::Kind Predictor::kindOf(const Instr &ins) {
  if (isBranch(ins.op))
    return COND;
  if (ins.rd == 1)
    return CALL;
  if (ins.op == Op::JALR && ins.rd == 0 && ins.rs1 == 1)
    return RET;
  return UNCOND;
}

uint32_t Predictor::phtIndex(uint32_t pc) const {
  return ((pc >> 2) ^ (nTables ? 0 : ghr)) & (uint32_t)(pht.size() - 1);
}

Predictor::BtbEntry *Predictor::btbFind(uint32_t pc) {
  BtbEntry *set = &btb[(size_t)((pc >> 2) % btbSets) * btbWays];
  for (uint32_t w = 0; w < btbWays; w++)
    if (set[w].valid && set[w].pc == pc) {
      set[w].lastUse = ++btbClock;
      return &set[w];
    }
  return nullptr;
}

// Folded history (Seznec): a width-bit compression of the newest len
// history bits, updated incrementally as one bit enters and one leaves
static inline uint16_t fold(uint16_t comp, uint32_t in, uint32_t out,
                            uint32_t len, uint32_t width) {
  uint32_t c = ((uint32_t)comp << 1) ^ in;
  c ^= out << (len % width);
  c ^= c >> width;
  return (uint16_t)(c & ((1u << width) - 1));
}

void Predictor::pushHistory(bool taken) {
  if (!nTables) {
    ghr = ((ghr << 1) | (taken ? 1 : 0)) & ghrMask;
    return;
  }
  const uint16_t *old = foldsAt(histPos);
  histPos++;
  histBits[histPos & ringMask] = taken;
  uint16_t *now = &folded[(size_t)(histPos & ringMask) * nTables * 3];
  for (uint32_t i = 0; i < nTables; i++) {
    const uint32_t out = histBits[(histPos - histLen[i]) & ringMask];
    now[3 * i] = fold(old[3 * i], taken, out, histLen[i], tableBits);
    now[3 * i + 1] = fold(old[3 * i + 1], taken, out, histLen[i], tagBits);
    now[3 * i + 2] = fold(old[3 * i + 2], taken, out, histLen[i], tagBits - 1);
  }
}

Predictor::Lookup Predictor::tageLookup(uint32_t pc, uint32_t pos) {
  const uint16_t *f = foldsAt(pos);
  const uint32_t p = pc >> 2, idxMask = (1u << tableBits) - 1,
                 tagMask = (1u << tagBits) - 1;
  Lookup l;
  for (uint32_t i = 0; i < nTables; i++) {
    idx[i] = (p ^ (p >> (tableBits - i % tableBits)) ^ f[3 * i]) & idxMask;
    tag[i] = (p ^ f[3 * i + 1] ^ ((uint32_t)f[3 * i + 2] << 1)) & tagMask;
  }
  for (int i = (int)nTables - 1; i >= 0; i--)
    if (tables[i][idx[i]].tag == tag[i]) {
      if (l.provider < 0)
        l.provider = i;
      else {
        l.alt = i;
        break;
      }
    }
  const bool base = pht[p & (pht.size() - 1)] >= 2;
  l.altPred = l.alt >= 0 ? tables[l.alt][idx[l.alt]].ctr >= 0 : base;
  if (l.provider < 0) {
    l.pred = l.providerPred = base;
    return l;
  }
  const int8_t c = tables[l.provider][idx[l.provider]].ctr;
  l.providerPred = c >= 0;
  l.weak = c == 0 || c == -1;
  l.pred = l.weak && useAltOnWeak >= 0 ? l.altPred : l.providerPred;
  return l;
}

// Move a saturating counter one step toward up/down within [lo, hi]
template <class T> static void nudge(T &c, bool up, int lo, int hi) {
  if (up && c < hi)
    c++;
  else if (!up && c > lo)
    c--;
}

void Predictor::tageUpdate(uint32_t pc, bool taken, uint32_t pos) {
  const Lookup l = tageLookup(pc, pos);
  uint8_t &base = pht[(pc >> 2) & (pht.size() - 1)];
  if (l.provider >= 0) {
    TageEntry &e = tables[l.provider][idx[l.provider]];
    // A fresh entry disagreeing with the alternative: learn which to trust
    if (l.weak && l.providerPred != l.altPred)
      nudge(useAltOnWeak, l.altPred == taken, -8, 7);
    if (l.providerPred != l.altPred)
      nudge(e.u, l.providerPred == taken, 0, 3);
    nudge(e.ctr, taken, -4, 3);
    // The base keeps learning while the provider is still unproven
    if (l.alt < 0 && l.weak)
      nudge(base, taken, 0, 3);
  } else {
    nudge(base, taken, 0, 3);
  }

  // A misprediction earns an entry in a longer-history table
  if (l.pred != taken && l.provider < (int)nTables - 1) {
    lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xb400u);
    int first = -1, second = -1;
    for (uint32_t j = (uint32_t)(l.provider + 1); j < nTables; j++)
      if (tables[j][idx[j]].u == 0) {
        if (first < 0)
          first = (int)j;
        else {
          second = (int)j;
          break;
        }
      }
    if (first < 0) {
      for (uint32_t j = (uint32_t)(l.provider + 1); j < nTables; j++)
        if (tables[j][idx[j]].u > 0)
          tables[j][idx[j]].u--;
    } else {
      const int j = second >= 0 && (lfsr & 3) == 0 ? second : first;
      TageEntry &e = tables[j][idx[j]];
      e.tag = (uint16_t)tag[j];
      e.ctr = taken ? 0 : -1;
      e.u = 0;
    }
  }
  // Age usefulness so stale entries eventually become replaceable
  if ((++updates & ((1u << 18) - 1)) == 0)
    for (auto &t : tables)
      for (TageEntry &e : t)
        e.u >>= 1;
}

Predictor::Pred Predictor::predict(uint32_t pc) {
  Pred p;
  p.before.hist = nTables ? histPos : ghr;
  p.before.rasN = rasN;
  p.before.rasTop = rasN ? ras[(rasN - 1) % ras.size()] : 0;
  p.phtIdx = phtIndex(pc);
  const BtbEntry *b = btbFind(pc);
  if (!b)
    return p; // unknown instruction: fall through
  switch (b->kind) {
  case COND:
    p.taken = nTables ? tageLookup(pc, histPos).pred : pht[p.phtIdx] >= 2;
    p.target = b->target;
    pushHistory(p.taken);
    break;
  case CALL:
    ras[rasN % ras.size()] = pc + 4;
    rasN++;
    p.taken = true;
    p.target = b->target;
    break;
  case RET:
    p.taken = true;
    if (rasN > 0) {
      rasN--;
      p.target = ras[rasN % ras.size()];
    } else {
      p.target = b->target; // empty stack: stale BTB target, best effort
    }
    break;
  case UNCOND:
    p.taken = true;
    p.target = b->target;
    break;
  }
  return p;
}

void Predictor::restore(const Checkpoint &cp) {
  // Drop the flushed wrong-path guesses
  if (nTables)
    histPos = cp.hist;
  else
    ghr = cp.hist;
  if (rasRepair) {
    rasN = cp.rasN;
    if (rasN)
      ras[(rasN - 1) % ras.size()] = cp.rasTop;
  }
}

void Predictor::replay(uint32_t pc, const Instr &ins, bool taken) {
  const Kind k = kindOf(ins);
  if (k == COND)
    pushHistory(taken);
  else if (rasRepair && k == CALL) {
    ras[rasN % ras.size()] = pc + 4;
    rasN++;
  } else if (rasRepair && k == RET && rasN > 0)
    rasN--;
}

void Predictor::update(uint32_t pc, const Instr &ins, bool taken,
                       uint32_t target, uint32_t phtIdx,
                       const Checkpoint &before) {
  const Kind k = kindOf(ins);
  if (k == COND) {
    if (nTables)
      tageUpdate(pc, taken, before.hist);
    else {
      uint8_t &c = pht[phtIdx]; // the counter the prediction consulted
      if (taken && c < 3)
        c++;
      else if (!taken && c > 0)
        c--;
    }
  }
  if (taken) { // allocate/refresh the BTB on taken control transfers
    BtbEntry *b = btbFind(pc);
    if (!b) { // replace the first free way, else the least recently used
      BtbEntry *set = &btb[(size_t)((pc >> 2) % btbSets) * btbWays];
      b = &set[0];
      for (uint32_t w = 0; w < btbWays; w++) {
        if (!set[w].valid) {
          b = &set[w];
          break;
        }
        if (set[w].lastUse < b->lastUse)
          b = &set[w];
      }
    }
    b->valid = true;
    b->pc = pc;
    b->target = target;
    b->kind = k;
    b->lastUse = ++btbClock;
  }
}
