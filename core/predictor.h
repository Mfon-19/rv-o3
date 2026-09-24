// The branch predictor: a direction predictor (gshare or TAGE), a branch
// target buffer, and a return-address stack.
//
// At fetch, each instruction address is looked up in the BTB. A miss
// predicts not-taken (fall through); the BTB is what tells fetch
// both that this IS a control instruction and where it goes, before
// decode. On a hit, the entry's kind decides: unconditional jumps and
// calls are taken; returns pop the RAS; conditional branches consult
// the direction predictor. The BTB is direct-mapped by default;
// btbWays > 1 makes it set-associative with LRU replacement.
//
// Direction predictors:
//   gshare (tageTables = 0): global history XOR pc indexes a 2-bit-counter
//     array. ghrBits = 0 degenerates it into a plain bimodal predictor.
//   TAGE (tageTables > 0): a bimodal base table plus tagged tables, each
//     indexed by the pc hashed with a longer slice of global history
//     (lengths grow geometrically from tageMinHist to tageMaxHist). The
//     longest-history table whose tag matches provides the prediction;
//     a newly allocated (weak) entry defers to the next match while a
//     small counter says that works better. Mispredictions allocate an
//     entry in a longer table; per-entry usefulness bits protect entries
//     that beat the alternative, and are periodically aged.
//
// The global history is updated at PREDICTION time, before the branch
// actually executes: each predicted conditional shifts its guessed
// direction into the history at fetch, so a loop sees the same history
// pattern every iteration. Since guesses can be wrong, every prediction
// carries a Checkpoint of the speculative state before its own update.
// A flush restores the checkpoint of the oldest squashed instruction,
// and a mispredicting instruction then replays its own effect with the
// real outcome. Training happens at commit: gshare uses the counter
// index carried from fetch; TAGE recomputes its table indices from the
// checkpointed history, so predict and train see the same history.
//
// TAGE history lives in a ring of bits with a monotonically increasing
// position. The folded (compressed) histories that form table indices
// and tags are stored per position, so a checkpoint is just a position:
// restoring it is O(1), and wrong-path bits past it are overwritten as
// fetch pushes again. The ring is sized to outlast every in-flight
// instruction plus the longest history.
//
// The return-address stack is updated on guesses. By default it is NOT
// repaired after a flush, so a squash can corrupt it, which costs only
// extra mispredictions later. rasRepair checkpoints its depth and top
// entry, the usual hardware fix, and restores them.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"
#include "sim/config.h"

class Predictor {
public:
  explicit Predictor(const SimConfig &cfg);

  // Speculative state before one prediction, for rewinding after a flush
  struct Checkpoint {
    uint32_t hist = 0; // gshare: history bits; TAGE: history position
    uint32_t rasN = 0, rasTop = 0;
  };

  struct Pred {
    bool taken = false;
    uint32_t target = 0;
    uint32_t phtIdx = 0; // gshare counter this prediction consulted
    Checkpoint before;
  };

  // Fetch-time lookup for the instruction at pc; the speculative RAS
  // push/pop and history shift happen here
  Pred predict(uint32_t pc);

  // Flush: rewind speculative state to a checkpoint
  void restore(const Checkpoint &cp);
  // After restore: apply a resolved instruction's own speculative effect
  // with its real outcome (history shift, RAS push/pop)
  void replay(uint32_t pc, const Instr &ins, bool taken);

  // Commit-time training with the resolved outcome
  void update(uint32_t pc, const Instr &ins, bool taken, uint32_t target,
              uint32_t phtIdx, const Checkpoint &before);

private:
  enum Kind : uint8_t { COND, UNCOND, CALL, RET };
  struct BtbEntry {
    bool valid = false;
    Kind kind = COND;
    uint32_t pc = 0, target = 0;
    uint64_t lastUse = 0; // LRU stamp (set-associative only)
  };

  static Kind kindOf(const Instr &ins);
  uint32_t phtIndex(uint32_t pc) const;
  BtbEntry *btbFind(uint32_t pc);
  void pushHistory(bool taken);

  const bool rasRepair;
  std::vector<uint8_t> pht; // gshare / TAGE base: 2-bit saturating counters
  uint32_t ghr = 0;         // gshare speculative global history
  uint32_t ghrMask;
  std::vector<BtbEntry> btb;
  uint32_t btbWays, btbSets;
  uint64_t btbClock = 0;
  std::vector<uint32_t> ras;
  uint32_t rasN = 0; // stack depth (wraps: oldest entries overwritten)

  // TAGE
  struct TageEntry {
    int8_t ctr = 0; // 3-bit signed: taken when >= 0
    uint8_t u = 0;  // 2-bit usefulness
    uint16_t tag = 0;
  };
  struct Lookup {
    int provider = -1, alt = -1; // table numbers; -1 = the base table
    bool providerPred = false, altPred = false, pred = false;
    bool weak = false;
  };
  const uint32_t nTables, tableBits, tagBits;
  std::vector<uint32_t> histLen;
  std::vector<std::vector<TageEntry>> tables;
  std::vector<uint8_t> histBits;   // ring of history bits
  std::vector<uint16_t> folded;    // per ring position: 3 folds per table
  uint32_t histPos = 0, ringMask = 0;
  int8_t useAltOnWeak = 0;         // 4-bit signed
  uint32_t updates = 0;            // usefulness aging clock
  uint32_t lfsr = 0xace1u;         // deterministic allocation choice
  // indices and tags per table for one lookup, filled by tageLookup
  std::vector<uint32_t> idx, tag;

  const uint16_t *foldsAt(uint32_t pos) const {
    return &folded[(size_t)(pos & ringMask) * nTables * 3];
  }
  Lookup tageLookup(uint32_t pc, uint32_t pos);
  void tageUpdate(uint32_t pc, bool taken, uint32_t pos);
};
