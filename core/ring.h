// Fixed-capacity circular buffer supporting head/tail pops and stable slot
// indices, used by the ROB, LSQ, store buffer, fetch queue, and free list.
// Internal storage rounds up to a power of two so index wrapping is a single
// bitmask with zero branches.

#pragma once

#include <bit>
#include <cstdint>
#include <vector>

template <class T>
class Ring {
public:
  explicit Ring(uint32_t capacity)
      : entries(std::bit_ceil(capacity ? capacity : 1u)),
        capacity(capacity),
        mask((uint32_t)entries.size() - 1) {}

  bool empty() const { return size == 0; }
  bool full() const { return size == capacity; }
  uint32_t count() const { return size; }

  uint32_t alloc() {
    const uint32_t idx = (headIndex + size) & mask;
    entries[idx] = T{};
    size++;
    return idx;
  }

  // Allocates a slot without zero-initializing it; caller must populate all fields.
  uint32_t allocRaw() {
    const uint32_t idx = (headIndex + size) & mask;
    size++;
    return idx;
  }

  void push(const T &value) {
    entries[(headIndex + size) & mask] = value;
    size++;
  }

  T &at(uint32_t idx) { return entries[idx]; }
  T &head() { return entries[headIndex]; }
  T &tail() { return entries[(headIndex + size - 1) & mask]; }
  T &nth(uint32_t offset) { return entries[(headIndex + offset) & mask]; }

  uint32_t indexOf(uint32_t offset) const {
    return (headIndex + offset) & mask;
  }

  // Returns true if `idx` currently belongs to the live [head, tail] range.
  bool live(uint32_t idx) const {
    return idx <= mask && ((idx - headIndex) & mask) < size;
  }

  void popHead() {
    headIndex = (headIndex + 1) & mask;
    size--;
  }

  void popTail() { size--; }

  void clear() {
    headIndex = 0;
    size = 0;
  }

private:
  std::vector<T> entries;
  const uint32_t capacity;
  const uint32_t mask;
  uint32_t headIndex = 0;
  uint32_t size = 0;
};
