#include "core/predictor.h"

Predictor::Predictor(const SimConfig &cfg)
    : pht(1u << cfg.phtBits, 1), // weakly not-taken
      ghrMask(cfg.ghrBits ? (1u << cfg.ghrBits) - 1 : 0),
      btb(cfg.btbEntries), ras(cfg.rasEntries) {}

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
  return ((pc >> 2) ^ ghr) & (uint32_t)(pht.size() - 1);
}

Predictor::Pred Predictor::predict(uint32_t pc) {
  Pred p;
  p.ghrBefore = ghr;
  p.phtIdx = phtIndex(pc);
  BtbEntry &b = btb[(pc >> 2) % btb.size()];
  if (!b.valid || b.pc != pc)
    return p; // unknown instruction: fall through
  switch (b.kind) {
  case COND:
    p.taken = pht[p.phtIdx] >= 2;
    p.target = b.target;
    ghr = ((ghr << 1) | (p.taken ? 1 : 0)) & ghrMask;
    break;
  case CALL:
    ras[rasN % ras.size()] = pc + 4;
    rasN++;
    p.taken = true;
    p.target = b.target;
    break;
  case RET:
    p.taken = true;
    if (rasN > 0) {
      rasN--;
      p.target = ras[rasN % ras.size()];
    } else {
      p.target = b.target; // empty stack: stale BTB target, best effort
    }
    break;
  case UNCOND:
    p.taken = true;
    p.target = b.target;
    break;
  }
  return p;
}

void Predictor::restore(uint32_t ghrBefore, bool cond, bool actualTaken) {
  // Drop the flushed wrong-path guesses from the history; a
  // conditional's own actual outcome goes back in
  ghr = cond ? (((ghrBefore << 1) | (actualTaken ? 1 : 0)) & ghrMask)
             : ghrBefore;
}

void Predictor::update(uint32_t pc, const Instr &ins, bool taken,
                       uint32_t target, uint32_t phtIdx) {
  const Kind k = kindOf(ins);
  if (k == COND) {
    uint8_t &c = pht[phtIdx]; // the counter the prediction consulted
    if (taken && c < 3)
      c++;
    else if (!taken && c > 0)
      c--;
  }
  if (taken) { // allocate/refresh the BTB on taken control transfers
    BtbEntry &b = btb[(pc >> 2) % btb.size()];
    b.valid = true;
    b.pc = pc;
    b.target = target;
    b.kind = k;
  }
}
