// The issue queue: where dispatched instructions wait for their
// operands and their functional unit.
//
// Select takes the OLDEST fully ready entries whose unit can accept, up
// to issue width; favoring the oldest keeps long dependency chains moving
// and makes starvation impossible.
//
// Entries live at their ROB slot, so visiting slots from the ROB head is
// program order. Their state is kept as bitmaps over those slots (one bit
// per slot, 64 per word) so wakeup and select work a word at a time
// instead of visiting every entry:
//   occupied     the slot holds a waiting instruction
//   blocked1/2   its source 1/2 is still waiting for a value
//   ready        occupied, and neither source blocked
//   wait1/2      one row per physical register p: the slots whose source
//                1/2 waits on p (row p is words [p*words, (p+1)*words))
// Writeback of p clears row p of wait1 and wait2, unblocks those slots,
// and marks the ones with nothing else outstanding ready. release()
// removes an entry from every bitmap when it issues or is squashed.

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
  bool valid = false;
};

struct IssueQueue {
  // Storage follows ROB slots; capacity still limits the number of waiting
  // instructions. ROB slots cannot be reused until their old op has retired.
  std::vector<IqEntry> e;
  IssueQueue(uint32_t capacity, uint32_t robSize, uint32_t physRegs)
      : e(robSize), capacity(capacity), words(((size_t)robSize + 63) / 64),
        occupied(words, 0), ready(words, 0),
        blocked1(words, 0), blocked2(words, 0),
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

  // Dispatch supplies each source's readiness once; a source that is not
  // ready is recorded against the register it waits on
  void track(IqEntry *q, bool ready1, bool ready2) {
    const size_t idx = (size_t)(q - e.data()), w = idx / 64;
    const uint64_t bit = uint64_t{1} << (idx % 64);
    if (!ready1) {
      wait1[(size_t)q->ps1 * words + w] |= bit;
      blocked1[w] |= bit;
    }
    if (!ready2) {
      wait2[(size_t)q->ps2 * words + w] |= bit;
      blocked2[w] |= bit;
    }
    if (ready1 && ready2)
      ready[w] |= bit;
  }

  bool sourceReady(uint32_t slot, bool second) const {
    return !((second ? blocked2 : blocked1)[slot / 64] &
             (uint64_t{1} << (slot % 64)));
  }

  void release(IqEntry *q) {
    const size_t idx = (size_t)(q - e.data()), w = idx / 64;
    const uint64_t bit = uint64_t{1} << (idx % 64);
    if (blocked1[w] & bit)
      wait1[(size_t)q->ps1 * words + w] &= ~bit;
    if (blocked2[w] & bit)
      wait2[(size_t)q->ps2 * words + w] &= ~bit;
    blocked1[w] &= ~bit;
    blocked2[w] &= ~bit;
    occupied[w] &= ~bit;
    ready[w] &= ~bit;
    q->valid = false;
    n--;
  }

  // Visit ready entries oldest first. The visitor may release the entry
  // it is given; returning false stops the walk
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
      // Wake every consumer with word operations, without fetching and
      // updating each IQ entry. Only newly unblocked entries become ready.
      blocked1[w] &= ~first;
      blocked2[w] &= ~second;
      ready[w] |= (first | second) & ~(blocked1[w] | blocked2[w]);
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
  std::vector<uint64_t> occupied, ready, blocked1, blocked2, wait1, wait2;
  uint32_t n = 0;
};
