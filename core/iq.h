// The issue queue: where dispatched instructions wait for their
// operands and their functional unit.
//
// Each entry tracks its two source physical registers and a ready bit
// per source. When a result writes back, its physical register number
// is broadcast to every entry here (the wakeup); any source waiting on
// that register flips to ready. Select takes the OLDEST fully ready
// entries whose unit can accept, up to issue width; favoring the
// oldest keeps long dependency chains moving and makes starvation
// impossible.

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "isa/isa.h"

struct IqEntry {
  uint64_t seq = 0;
  // Decoded instruction and PC live in the ROB, not in a second copy here.
  uint32_t robIdx = 0, lsqIdx = 0;
  uint8_t ps1 = 0, ps2 = 0;
  FuKind unit = FuKind::NONE;
  bool ready1 = true, ready2 = true;
  bool valid = false;
};

struct IssueQueue {
  // Storage follows ROB slots; capacity still limits the number of waiting
  // instructions. ROB slots cannot be reused until their old op has retired.
  std::vector<IqEntry> e;
  IssueQueue(uint32_t capacity, uint32_t robSize, uint32_t physRegs)
      : e(robSize), capacity(capacity), words(((size_t)robSize + 63) / 64),
        occupied(words, 0), ready(words, 0),
        wait1((size_t)physRegs * words, 0), wait2(wait1.size(), 0) {}

  bool full() const { return n == capacity; }
  uint32_t count() const { return n; }

  IqEntry *allocate(uint32_t robIdx) {
    if (full())
      return nullptr;
    IqEntry &q = e[robIdx];
    q = IqEntry{};
    q.valid = true;
    q.robIdx = robIdx;
    occupied[robIdx / 64] |= uint64_t{1} << (robIdx % 64);
    n++;
    return &q;
  }

  // Dispatch calls this once, after filling the source registers and ready
  // flags. Waiter masks let writeback visit only consumers of its register.
  void track(IqEntry *q) {
    const size_t idx = (size_t)(q - e.data()), w = idx / 64;
    const uint64_t bit = uint64_t{1} << (idx % 64);
    if (!q->ready1)
      wait1[(size_t)q->ps1 * words + w] |= bit;
    if (!q->ready2)
      wait2[(size_t)q->ps2 * words + w] |= bit;
    if (q->ready1 && q->ready2)
      ready[w] |= bit;
  }

  void release(IqEntry *q) {
    const size_t idx = (size_t)(q - e.data()), w = idx / 64;
    const uint64_t bit = uint64_t{1} << (idx % 64);
    if (!q->ready1)
      wait1[(size_t)q->ps1 * words + w] &= ~bit;
    if (!q->ready2)
      wait2[(size_t)q->ps2 * words + w] &= ~bit;
    occupied[w] &= ~bit;
    ready[w] &= ~bit;
    q->valid = false;
    n--;
  }

  // The visitor may release its current entry. False stops after the last
  // issue winner, avoiding work on younger entries that cannot issue today.
  template <class Visitor> void visitReady(uint32_t robHead, Visitor visit) {
    // Visit the head word's upper bits, wrap through the remaining words,
    // then its lower bits. This is program order without sorting.
    const size_t headWord = robHead / 64;
    const uint64_t upper = UINT64_MAX << (robHead % 64);
    if (!visitWord(headWord, ready[headWord] & upper, visit))
      return;
    for (size_t word = headWord + 1; word < words; word++)
      if (!visitWord(word, ready[word], visit))
        return;
    for (size_t word = 0; word < headWord; word++)
      if (!visitWord(word, ready[word], visit))
        return;
    visitWord(headWord, ready[headWord] & ~upper, visit);
  }

  void wakeup(uint8_t preg) {
    for (size_t w = 0; w < words; w++) {
      const size_t idx = (size_t)preg * words + w;
      const uint64_t first = wait1[idx], second = wait2[idx];
      wait1[idx] = wait2[idx] = 0;
      uint64_t bits = first | second;
      while (bits) {
        const unsigned b = __builtin_ctzll(bits);
        const uint64_t bit = uint64_t{1} << b;
        IqEntry &q = e[w * 64 + b];
        if (first & bit)
          q.ready1 = true;
        if (second & bit)
          q.ready2 = true;
        if (q.ready1 && q.ready2)
          ready[w] |= bit;
        bits &= bits - 1;
      }
    }
  }

  void flushYounger(uint64_t seq) {
    for (size_t w = 0; w < words; w++) {
      uint64_t bits = occupied[w];
      while (bits) {
        IqEntry &q = e[w * 64 + __builtin_ctzll(bits)];
        if (q.seq > seq)
          release(&q);
        bits &= bits - 1;
      }
    }
  }

private:
  template <class Visitor>
  bool visitWord(size_t word, uint64_t bits, Visitor &visit) {
    while (bits) {
      if (!visit(e[word * 64 + __builtin_ctzll(bits)]))
        return false;
      bits &= bits - 1;
    }
    return true;
  }

  uint32_t capacity;
  size_t words;
  std::vector<uint64_t> occupied, ready, wait1, wait2;
  uint32_t n = 0;
};
