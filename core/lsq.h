// The load/store queue and the store buffer — conservative memory
// ordering.
//
// Memory ops allocate an LSQ entry at dispatch, in program order. The
// AGU fills in the address (and, for stores, the data captured at
// issue) when the operands are ready. The ordering rules, deliberately
// conservative for now:
//
//   - A load may access the cache only when EVERY older store in the
//     queue has a known address.
//   - An older store to exactly the load's address and size forwards
//     its data; a partial overlap makes the load wait until that
//     store has drained to the cache.
//   - Stores touch memory only after they commit: commit moves them
//     to the store buffer, which drains to the data port in order.
//
// No memory-dependence speculation, no load reordering past unknown
// store addresses — memory bugs must not be able to hide behind
// renaming bugs while the out-of-order engine is being proven.

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
  uint32_t data = 0; // stores: the (full-width) data to write
  bool done = false; // loads: value produced (forwarded or from cache)
  bool reported = false; // loads: handed to the writeback arbiter
  uint32_t value = 0;
};

class LSQ {
public:
  explicit LSQ(uint32_t size) : e(size) {}

  bool empty() const { return n == 0; }
  bool full() const { return n == e.size(); }
  uint32_t count() const { return n; }

  uint32_t alloc() {
    uint32_t idx = (headIdx + n) % (uint32_t)e.size();
    e[idx] = LsqEntry{};
    n++;
    return idx;
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
  uint32_t headIdx = 0;
  uint32_t n = 0;
};

// Committed stores waiting to be written to the cache, drained in
// order. A full store buffer stalls commit (and gets priority for the
// data port, so it always drains eventually)
struct StoreBufEntry {
  uint32_t addr = 0;
  uint8_t size = 4;
  uint32_t data = 0;
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
