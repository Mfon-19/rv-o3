// Fixed-capacity circular buffer supporting head/tail pops and stable slot
// indices, used by the ROB, LSQ, store buffer, fetch queue, and free list.

#pragma once

#include <cstdint>
#include <vector>

template <class T>
class Ring {
public:
  explicit Ring(uint32_t capacity) : entries(capacity), capacity(capacity) {}

  bool empty() const { return size == 0; }
  bool full() const { return size == capacity; }
  uint32_t count() const { return size; }

  uint32_t alloc() {
    const uint32_t idx = indexOf(size);
    entries[idx] = T{};
    size++;
    return idx;
  }

  void push(const T &value) { entries[alloc()] = value; }

  T &at(uint32_t idx) { return entries[idx]; }
  T &head() { return entries[headIndex]; }
  T &tail() { return entries[indexOf(size - 1)]; }
  T &nth(uint32_t offset) { return entries[indexOf(offset)]; }

  uint32_t indexOf(uint32_t offset) const {
    const uint32_t toEnd = capacity - headIndex;
    return offset < toEnd ? headIndex + offset : offset - toEnd;
  }

  // Returns true if `idx` currently belongs to the live [head, tail] range.
  bool live(uint32_t idx) const {
    const uint32_t distance =
        idx >= headIndex ? idx - headIndex : capacity - (headIndex - idx);
    return idx < capacity && distance < size;
  }

  void popHead() {
    headIndex = indexOf(1);
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
  uint32_t headIndex = 0;
  uint32_t size = 0;
};
