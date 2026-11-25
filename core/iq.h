// Age-ordered issue queue holding dispatched instructions waiting for source
// physical registers to become ready. For capacities <= 64, stable slot
// indices with per-register 64-bit waiter masks provide O(1) wakeups and
// zero-copy issue without compacting IqEntry structs.

#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

#include "isa/isa.h"

struct IqEntry {
  uint64_t seq = 0;
  uint16_t robIdx = 0;
  uint16_t lsqIdx = 0;
  uint8_t src1PhysReg = 0;
  uint8_t src2PhysReg = 0;
  bool ready1 = true;
  bool ready2 = true;
  FuKind unit = FuKind::NONE;
};

struct IssueQueue {
  std::vector<IqEntry> entries;
  uint32_t capacity = 0;
  uint32_t entryCount = 0;
  uint32_t readyCount = 0;

  // Fast-path bitmask state used when capacity <= 64.
  uint64_t occupiedMask = 0;
  uint64_t readyMask = 0;
  std::array<uint8_t, 64> ageOrder{};
  std::array<uint64_t, 256> regWaitMask{};

  // Fallback waiter counts used when capacity > 64.
  std::array<uint16_t, 256> regWaiters{};

  explicit IssueQueue(uint32_t capacity) : capacity(capacity) {
    if (capacity <= 64)
      entries.resize(capacity);
    else
      entries.reserve(capacity);
  }

  bool usesBitmask() const { return capacity <= 64; }
  bool full() const { return count() >= capacity; }
  uint32_t count() const { return usesBitmask() ? entryCount : (uint32_t)entries.size(); }

  void push(const IqEntry &entry) {
    if (usesBitmask()) {
      const uint32_t slot = (uint32_t)std::countr_zero(~occupiedMask);
      const uint64_t bit = 1ull << slot;
      entries[slot] = entry;
      occupiedMask |= bit;
      ageOrder[entryCount++] = (uint8_t)slot;

      if (!entry.ready1)
        regWaitMask[entry.src1PhysReg] |= bit;
      if (!entry.ready2)
        regWaitMask[entry.src2PhysReg] |= bit;
      if (entry.ready1 && entry.ready2) {
        readyMask |= bit;
        readyCount++;
      }
      return;
    }

    entries.push_back(entry);
    if (!entry.ready1)
      regWaiters[entry.src1PhysReg]++;
    if (!entry.ready2)
      regWaiters[entry.src2PhysReg]++;
    if (entry.ready1 && entry.ready2)
      readyCount++;
  }

  [[gnu::always_inline]] inline void wakeupBitmask(uint8_t physReg) {
    uint64_t waiters = regWaitMask[physReg];
    if (!waiters) [[likely]]
      return;

    regWaitMask[physReg] = 0;

    do {
      const uint32_t slot = (uint32_t)std::countr_zero(waiters);
      waiters &= waiters - 1;
      IqEntry &entry = entries[slot];

      if (entry.src1PhysReg == physReg)
        entry.ready1 = true;
      if (entry.src2PhysReg == physReg)
        entry.ready2 = true;

      if (entry.ready1 && entry.ready2) {
        readyMask |= (1ull << slot);
        readyCount++;
      }
    } while (waiters);
  }

  void wakeup(uint8_t physReg) {
    if (usesBitmask()) {
      wakeupBitmask(physReg);
      return;
    }

    uint16_t remaining = regWaiters[physReg];
    if (remaining == 0)
      return;

    for (IqEntry &entry : entries) {
      if (!entry.ready1 && entry.src1PhysReg == physReg) {
        entry.ready1 = true;
        if (entry.ready2)
          readyCount++;
        if (--remaining == 0)
          break;
      }

      if (!entry.ready2 && entry.src2PhysReg == physReg) {
        entry.ready2 = true;
        if (entry.ready1)
          readyCount++;
        if (--remaining == 0)
          break;
      }
    }

    regWaiters[physReg] = remaining;
  }

  void releaseSlot(uint32_t slot) {
    const uint64_t bit = 1ull << slot;
    occupiedMask &= ~bit;
    readyMask &= ~bit;
    readyCount--;
  }

  void flushYounger(uint64_t keepSeq) {
    if (usesBitmask()) {
      while (entryCount > 0) {
        const uint32_t slot = ageOrder[entryCount - 1];
        const IqEntry &entry = entries[slot];
        if (entry.seq <= keepSeq)
          break;

        entryCount--;
        const uint64_t bit = 1ull << slot;
        occupiedMask &= ~bit;

        if (readyMask & bit) {
          readyMask &= ~bit;
          readyCount--;
        }
        if (!entry.ready1)
          regWaitMask[entry.src1PhysReg] &= ~bit;
        if (!entry.ready2)
          regWaitMask[entry.src2PhysReg] &= ~bit;
      }
      return;
    }

    while (!entries.empty() && entries.back().seq > keepSeq) {
      const IqEntry &entry = entries.back();
      if (!entry.ready1)
        regWaiters[entry.src1PhysReg]--;
      if (!entry.ready2)
        regWaiters[entry.src2PhysReg]--;
      if (entry.ready1 && entry.ready2)
        readyCount--;
      entries.pop_back();
    }
  }
};
