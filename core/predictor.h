// The branch predictor: gshare direction prediction, a branch target
// buffer, and a return-address stack.
//
// At fetch, each instruction address is looked up in the BTB. A miss
// predicts not-taken (fall through); the BTB is what tells fetch
// both that this IS a control instruction and where it goes, before
// decode. On a hit, the entry's kind decides: unconditional jumps and
// calls are taken; returns pop the RAS; conditional branches consult
// the gshare table (global history XOR pc indexing a 2-bit-counter
// array). ghrBits = 0 degenerates gshare into a plain bimodal
// predictor.
//
// The global history is updated at PREDICTION time, before the branch
// actually executes: each predicted conditional shifts its guessed
// direction into the history register at fetch, so a loop sees the
// same history pattern every iteration. Since guesses can be wrong,
// every prediction carries a snapshot of the history before its shift
// plus its table index; a mispredicting branch restores the history
// from that snapshot (with the true outcome shifted in), and training
// at commit uses the carried index, so predict and train always touch
// the same counter.
//
// The return-address stack is also updated on guesses, and is NOT
// repaired after a flush; a squash can corrupt it, which costs only
// extra mispredictions later.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"
#include "sim/config.h"

class Predictor {
public:
  explicit Predictor(const SimConfig &cfg);

  struct Pred {
    bool taken = false;
    uint32_t target = 0;
    uint32_t phtIdx = 0;    // counter this prediction consulted
    uint32_t ghrBefore = 0; // history before this prediction shifted it
  };

  // Fetch-time lookup for the instruction at pc; the speculative RAS
  // push/pop and GHR shift happen here
  Pred predict(uint32_t pc);

  // Recovery: rewind the history to a recovery point's snapshot, with
  // the actual outcome shifted in when that point is a conditional
  void restore(uint32_t ghrBefore, bool cond, bool actualTaken);

  // Commit-time training with the resolved outcome
  void update(uint32_t pc, const Instr &ins, bool taken, uint32_t target,
              uint32_t phtIdx);

private:
  enum Kind : uint8_t { COND, UNCOND, CALL, RET };
  struct BtbEntry {
    bool valid = false;
    uint32_t pc = 0, target = 0;
    Kind kind = COND;
  };

  static Kind kindOf(const Instr &ins);
  uint32_t phtIndex(uint32_t pc) const;

  std::vector<uint8_t> pht; // 2-bit saturating counters
  uint32_t ghr = 0;         // speculative global history
  uint32_t ghrMask;
  std::vector<BtbEntry> btb;
  std::vector<uint32_t> ras;
  uint32_t rasN = 0; // stack depth (wraps: oldest entries overwritten)
};
