// A fixed-capacity ring of entries, oldest at the head.
//
// The reorder buffer, the load/store queue, the store buffer, and the
// free list are all this shape: allocate at the tail, retire from the
// head, squash from the tail. An entry's ring index stays stable while
// it is live even as the head advances, which is what lets in-flight
// ops name their ROB and LSQ entries by index.

#pragma once

#include <cstdint>
#include <vector>

template <class T> class Ring {
public:
  explicit Ring(uint32_t capacity) : e(capacity), capacity(capacity) {}

  bool empty() const { return n == 0; }
  bool full() const { return n == capacity; }
  uint32_t count() const { return n; }

  // Take the slot after the tail, reset it, and return its ring index
  uint32_t alloc() {
    const uint32_t idx = indexOf(n);
    e[idx] = T{};
    n++;
    return idx;
  }
  void push(const T &v) { e[alloc()] = v; }

  T &at(uint32_t idx) { return e[idx]; }
  T &head() { return e[headIdx]; }
  T &tail() { return e[indexOf(n - 1)]; }
  T &nth(uint32_t k) { return e[indexOf(k)]; } // k-th oldest, 0 = head
  uint32_t indexOf(uint32_t k) const {
    // All callers use offsets <= capacity: at most one wrap, even for
    // non-power-of-two capacities. Subtract first to avoid overflow.
    const uint32_t toEnd = capacity - headIdx;
    return k < toEnd ? headIdx + k : k - toEnd;
  }
  // Is this ring index currently occupied by a live entry?
  bool live(uint32_t idx) const {
    const uint32_t distance = idx >= headIdx ? idx - headIdx
                                            : capacity - (headIdx - idx);
    return idx < capacity && distance < n;
  }

  void popHead() {
    headIdx = indexOf(1);
    n--;
  }
  void popTail() { n--; }
  void clear() { headIdx = n = 0; }

private:
  std::vector<T> e;
  const uint32_t capacity;
  uint32_t headIdx = 0, n = 0;
};
