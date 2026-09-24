// A set of ring slots, stored as a bitmap, that can be visited in ring age
// order: starting from the ring's head slot and wrapping around, so the
// oldest member comes first even when the ring has wrapped. The LSQ keeps
// one per question it asks every cycle (core/lsq.h).
//
// Visitors receive slot numbers, may remove the slot they are given, and
// return false to stop the walk.
#pragma once

#include <cstdint>
#include <vector>

class SlotSet {
public:
  explicit SlotSet(uint32_t capacity)
      : capacity(capacity), words((uint64_t(capacity) + 63) / 64, 0) {}
  void set(uint32_t slot) { words[slot / 64] |= uint64_t(1) << (slot % 64); }
  void reset(uint32_t slot) { words[slot / 64] &= ~(uint64_t(1) << (slot % 64)); }

  template <class F> void visit(uint32_t head, F &&f) const {
    if (capacity <= 64) {
      // Common queues fit in one word. Keep the two halves separate so
      // removals made while visiting the upper half affect the lower half.
      if (!forwardBits(words[0] & (UINT64_MAX << head), 0, f)) return;
      if (head) forwardBits(words[0] & ((uint64_t(1) << head) - 1), 0, f);
      return;
    }
    if (forward(head, capacity, f)) forward(0, head, f);
  }
  // Only slots strictly older than end, newest first.
  template <class F> void visitOlder(uint32_t head, uint32_t end, F &&f) const {
    if (end >= head) reverse(head, end, f);
    else if (reverse(0, end, f)) reverse(head, capacity, f);
  }

private:
  uint32_t capacity;
  std::vector<uint64_t> words;
  static uint64_t below(unsigned bit) { return bit ? (uint64_t(1) << bit) - 1 : UINT64_MAX; }
  template <class F> static bool forwardBits(uint64_t bits, uint32_t base, F &f) {
    while (bits) {
      const uint32_t slot = base + __builtin_ctzll(bits);
      bits &= bits - 1;
      if (!f(slot)) return false;
    }
    return true;
  }
  template <class F> bool forward(uint32_t begin, uint32_t end, F &f) const {
    if (begin == end) return true;
    const uint32_t last = (end - 1) / 64;
    for (uint32_t w = begin / 64; w <= last; w++) {
      uint64_t bits = words[w];
      if (w == begin / 64) bits &= UINT64_MAX << (begin % 64);
      if (w == last) bits &= below(end % 64);
      if (!forwardBits(bits, w * 64, f)) return false;
    }
    return true;
  }
  template <class F> bool reverse(uint32_t begin, uint32_t end, F &f) const {
    if (begin == end) return true;
    const uint32_t first = begin / 64, last = (end - 1) / 64;
    for (uint32_t w = last;; w--) {
      uint64_t bits = words[w];
      if (w == first) bits &= UINT64_MAX << (begin % 64);
      if (w == last) bits &= below(end % 64);
      while (bits) {
        const unsigned bit = 63 - __builtin_clzll(bits);
        bits &= ~(uint64_t(1) << bit);
        if (!f(w * 64 + bit)) return false;
      }
      if (w == first) break;
    }
    return true;
  }
};
