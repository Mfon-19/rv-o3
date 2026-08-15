// The issue queue: where dispatched instructions wait for their
// operands and their functional unit.
//
// Each entry tracks its two source physical registers and a ready bit
// per source. When a result writes back, its physical register number
// is broadcast here (wakeup) and matching sources flip ready. The
// scheduler picks the OLDEST ready entries whose unit can accept, up
// to issue width — oldest-first keeps long-latency chains moving and
// makes starvation impossible.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"

struct IqEntry {
  bool valid = false;
  uint64_t seq = 0;
  uint32_t robIdx = 0, lsqIdx = 0;
  uint32_t pc = 0;
  Instr ins;
  uint8_t pdst = 0xFF;
  uint8_t ps1 = 0, ps2 = 0;
  bool ready1 = true, ready2 = true;
};

struct IssueQueue {
  std::vector<IqEntry> e;
  explicit IssueQueue(uint32_t size) : e(size) {}

  bool full() const {
    for (const IqEntry &q : e)
      if (!q.valid)
        return false;
    return true;
  }
  uint32_t count() const {
    uint32_t n = 0;
    for (const IqEntry &q : e)
      n += q.valid;
    return n;
  }

  IqEntry *allocate() {
    for (IqEntry &q : e)
      if (!q.valid) {
        q = IqEntry{};
        q.valid = true;
        return &q;
      }
    return nullptr; // caller checked full()
  }

  // Result broadcast: wake every source waiting on this register
  void wakeup(uint8_t preg) {
    for (IqEntry &q : e) {
      if (!q.valid)
        continue;
      if (q.ps1 == preg)
        q.ready1 = true;
      if (q.ps2 == preg)
        q.ready2 = true;
    }
  }

  // The oldest ready entry the scheduler hasn't taken this cycle
  IqEntry *oldestReady() {
    IqEntry *best = nullptr;
    for (IqEntry &q : e)
      if (q.valid && q.ready1 && q.ready2 &&
          (!best || q.seq < best->seq))
        best = &q;
    return best;
  }

  void flushYounger(uint64_t seq) {
    for (IqEntry &q : e)
      if (q.valid && q.seq > seq)
        q.valid = false;
  }
};
