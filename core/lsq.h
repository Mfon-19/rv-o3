// The load/store queue and the store buffer; together they carry the
// memory-ordering rules.
//
// Memory ops allocate an LSQ entry at dispatch, in program order. The
// AGU fills in the address when the address operand is ready; a
// store's data arrives separately, whenever its producer writes back.
// The rules every mode shares:
//
//   - An older store to exactly the load's address and size hands the
//     load its data directly (a forward); a partial overlap makes the
//     load wait until that store has drained to the cache.
//   - Stores touch memory only after they commit: commit moves them
//     to the store buffer, which drains to the data port in order.
//
// How far a load may run ahead of older stores whose addresses are
// not known yet is the configurable part (SimConfig::memOrder): wait
// for all of them (Conservative), pass only stores proven not to
// overlap (Bypass), or guess that unknown addresses will not overlap
// and re-execute the load if the guess was wrong (Speculative, the
// default; the re-execution is called a replay and reuses the branch
// recovery machinery).

#pragma once

#include <cstdint>
#include <vector>

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
  // physical register is watched until it's ready (its producer writes
  // it exactly once, so a late capture is always safe)
  uint32_t data = 0;
  uint8_t dataPreg = 0xFF;
  bool dataReady = true;
  bool done = false; // loads: value produced (forwarded or from cache)
  bool issued = false;   // loads: access in flight at the cache
  bool reported = false; // loads: handed to the writeback arbiter
  uint16_t gen = 0;      // slot generation, embedded in the access tag
                         // so a squashed load's response fails the match
  uint32_t value = 0;
};

class LSQ {
public:
  explicit LSQ(uint32_t size) : e(size), genCtr(size, 0) {}

  bool empty() const { return n == 0; }
  bool full() const { return n == e.size(); }
  uint32_t count() const { return n; }

  uint32_t alloc() {
    uint32_t idx = (headIdx + n) % (uint32_t)e.size();
    e[idx] = LsqEntry{};
    e[idx].gen = ++genCtr[idx]; // stale responses to this slot die here
    n++;
    return idx;
  }

  // Is this ring slot currently occupied by a live entry?
  bool live(uint32_t idx) const {
    return (idx + (uint32_t)e.size() - headIdx) % (uint32_t)e.size() < n;
  }

  LsqEntry &at(uint32_t idx) { return e[idx]; }
  LsqEntry &head() { return e[headIdx]; }
  LsqEntry &tail() { return e[(headIdx + n - 1) % (uint32_t)e.size()]; }
  // k-th oldest, 0 = head
  LsqEntry &nth(uint32_t k) { return e[(headIdx + k) % (uint32_t)e.size()]; }
  // the stable ring index of the k-th oldest entry (what FuOps carry)
  uint32_t indexOf(uint32_t k) const {
    return (headIdx + k) % (uint32_t)e.size();
  }

  void popHead() {
    headIdx = (headIdx + 1) % (uint32_t)e.size();
    n--;
  }
  void popTail() { n--; }

private:
  std::vector<LsqEntry> e;
  std::vector<uint16_t> genCtr;
  uint32_t headIdx = 0;
  uint32_t n = 0;
};

// Committed stores waiting to be written to the cache, issued in
// order, several in flight at once; entries stay (visible to load
// forwarding) until the cache acknowledges, and pop in order. A full
// store buffer stalls commit (and gets priority for the data port, so
// it always drains eventually)
struct StoreBufEntry {
  uint32_t addr = 0;
  uint8_t size = 4;
  uint32_t data = 0;
  bool inflight = false; // issued to the cache
  bool acked = false;    // cache applied it; pop when it reaches head
  uint32_t txn = 0;      // matches the ack to this entry
};

class StoreBuffer {
public:
  explicit StoreBuffer(uint32_t size) : e(size) {}
  bool empty() const { return n == 0; }
  bool full() const { return n == e.size(); }
  uint32_t count() const { return n; }
  void push(const StoreBufEntry &s) {
    e[(headIdx + n) % (uint32_t)e.size()] = s;
    n++;
  }
  StoreBufEntry &head() { return e[headIdx]; }
  StoreBufEntry &nth(uint32_t k) { return e[(headIdx + k) % (uint32_t)e.size()]; }
  void popHead() {
    headIdx = (headIdx + 1) % (uint32_t)e.size();
    n--;
  }

private:
  std::vector<StoreBufEntry> e;
  uint32_t headIdx = 0;
  uint32_t n = 0;
};
