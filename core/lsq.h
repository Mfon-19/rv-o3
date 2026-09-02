// The load/store queue and the store buffer; together they carry the
// memory-ordering rules.
//
// Memory ops allocate an LSQ entry at dispatch, in program order. The
// AGU fills in the address when the address operand is ready; a
// store's data arrives separately, whenever its producer writes back.
// The rules every mode shares:
//
//   - An older store to exactly the load's address and size hands the
//     load its data directly (a forward). A partial overlap makes the
//     load wait until that store has drained to the cache; there is no
//     byte merging across stores.
//   - Stores touch memory only after they commit: commit moves them
//     to the store buffer, which drains to the data port in order.
//
// How far a load may run ahead of older stores whose addresses are
// not known yet is the configurable part (SimConfig::memOrder): wait
// for all of them (Conservative, which also issues loads one at a
// time), pass only stores proven not to overlap (Bypass), or guess
// that unknown addresses will not overlap and re-execute the load if
// the guess was wrong (Speculative, the default). The re-execution is
// called a replay and reuses the branch recovery machinery.

#pragma once

#include <cstdint>
#include <vector>

#include "core/ring.h"
#include "isa/isa.h"

struct LsqEntry {
  uint64_t seq = 0;
  uint32_t robIdx = 0;
  uint32_t pc = 0;
  Instr ins;
  bool isStore = false;
  uint8_t pdst = 0xFF; // loads: where the result goes
  bool addrValid = false;
  uint32_t addr = 0;
  uint8_t size = 4;
  // Stores: address and data resolve independently. The data operand's
  // physical register is watched until it is ready; it is written
  // exactly once while this store is live, so a late capture is
  // always safe
  uint32_t data = 0;
  uint8_t dataPreg = 0xFF;
  bool dataReady = true;
  bool done = false;     // loads: value produced (forwarded or from cache)
  bool issued = false;   // loads: access in flight at the cache
  bool reported = false; // loads: handed to the writeback arbiter
  uint16_t gen = 0;      // slot generation, embedded in the access tag
                         // so a squashed load's response fails the match
  uint32_t value = 0;
};

// Do two accesses touch any byte in common?
inline bool overlaps(uint32_t a, uint32_t aSize, uint32_t b, uint32_t bSize) {
  return a + aSize > b && b + bSize > a;
}

// A ring of LsqEntry whose slots carry a generation number, so the
// response to a squashed load's cache access cannot be mistaken for
// the slot's next occupant's
class LSQ : public Ring<LsqEntry> {
public:
  explicit LSQ(uint32_t size) : Ring(size), genCtr(size, 0) {}

  uint32_t alloc() {
    const uint32_t idx = Ring::alloc();
    at(idx).gen = ++genCtr[idx]; // stale responses to this slot die here
    return idx;
  }

private:
  std::vector<uint16_t> genCtr;
};

// Committed stores waiting to be written to the cache: issued in
// order, several in flight at once. Entries stay (visible to load
// forwarding) until the cache acknowledges, and pop in order. A full
// store buffer stalls commit and gets priority for the data port, so
// it always drains eventually
struct StoreBufEntry {
  uint32_t addr = 0;
  uint8_t size = 4;
  uint32_t data = 0;
  bool inflight = false; // issued to the cache
  bool acked = false;    // cache applied it; pop when it reaches head
  uint32_t txn = 0;      // matches the ack to this entry
};

using StoreBuffer = Ring<StoreBufEntry>;
