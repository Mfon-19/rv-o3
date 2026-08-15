// The retire queue: in-order retirement over out-of-order completion.
//
// With multicycle functional units, results complete out of program
// order (a divide finishes long after younger ALU work has written
// back). The commit stream, though, must stay in program order for the
// differential checker — and later for precise state. So every
// instruction allocates an entry here at issue, in program order; the
// writeback stage marks entries done as results arrive; and retirement
// consumes only from the head, emitting CommitRecords strictly in
// order. This is a reorder buffer minus register renaming — the
// out-of-order core later grows it into the real thing.

#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "core/commit.h"
#include "isa/isa.h"

struct RetireEntry {
  bool done = false; // result written back; head may retire
  uint32_t pc = 0;
  Instr ins;
  bool hasRegWrite = false; // filled by writeback
  uint32_t value = 0;
  std::optional<MemoryWrite> memWrite;
};

class RetireQueue {
public:
  explicit RetireQueue(uint32_t size) : e(size) {}

  bool empty() const { return n == 0; }
  bool full() const { return n == e.size(); }
  uint32_t count() const { return n; }
  uint32_t size() const { return (uint32_t)e.size(); }

  // Allocate the next entry in program order; returns its index, which
  // in-flight ops carry so writeback can find the entry again
  uint32_t alloc(uint32_t pc, const Instr &ins) {
    uint32_t idx = (headIdx + n) % (uint32_t)e.size();
    e[idx] = RetireEntry{};
    e[idx].pc = pc;
    e[idx].ins = ins;
    n++;
    return idx;
  }

  RetireEntry &at(uint32_t idx) { return e[idx]; }
  RetireEntry &head() { return e[headIdx]; }
  void pop() {
    headIdx = (headIdx + 1) % (uint32_t)e.size();
    n--;
  }

private:
  std::vector<RetireEntry> e;
  uint32_t headIdx = 0;
  uint32_t n = 0;
};
