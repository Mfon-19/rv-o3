// Age-ordered issue queue holding dispatched instructions waiting for source
// physical registers to become ready.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"

struct IqEntry {
  uint64_t seq = 0;
  uint32_t robIdx = 0;
  uint32_t lsqIdx = 0;
  uint8_t src1PhysReg = 0;
  uint8_t src2PhysReg = 0;
  bool ready1 = true;
  bool ready2 = true;
  FuKind unit = FuKind::NONE;
};

struct IssueQueue {
  // Entries stay sorted oldest-first (by dispatch seq) so scanning index 0..N
  // naturally implements oldest-first issue priority.
  std::vector<IqEntry> entries;
  uint32_t capacity = 0;

  explicit IssueQueue(uint32_t capacity) : capacity(capacity) {
    entries.reserve(capacity);
  }

  bool full() const { return entries.size() >= capacity; }
  uint32_t count() const { return (uint32_t)entries.size(); }

  void push(const IqEntry &entry) { entries.push_back(entry); }

  void wakeup(uint8_t physReg) {
    for (IqEntry &entry : entries) {
      if (!entry.ready1 && entry.src1PhysReg == physReg)
        entry.ready1 = true;
      if (!entry.ready2 && entry.src2PhysReg == physReg)
        entry.ready2 = true;
    }
  }

  void flushYounger(uint64_t keepSeq) {
    while (!entries.empty() && entries.back().seq > keepSeq)
      entries.pop_back();
  }
};
