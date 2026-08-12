// Main memory: flat, byte addressable, little-endian, zero-initialized
//
// A single unified memory that holds both instructions and data. The
// current five-stage core pretends IF and MEM use separate ports, so
// there is never a structural hazard between fetch and load, and every
// access completes in the cycle it is issued.
//
// This directory will grow the timed memory system (request/response
// ports, caches, DRAM models); this flat array will remain at the
// bottom of that hierarchy as the backing store.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct Memory {
  std::vector<uint8_t> bytes;

  explicit Memory(size_t size) : bytes(size, 0) {}

  // Any out-of-range access is a fatal simulation error. On real hardware,
  // an access-fault exception would be raised
  void check(uint32_t addr, uint32_t len, const char *what) const {
    if ((uint64_t)addr + len > bytes.size()) {
      fprintf(stderr, "fatal: %s at 0x%08x is outside memory (%zu bytes)\n",
              what, addr, bytes.size());
      exit(1);
    }
  }

  // load/store for different data sizes
  uint8_t load8(uint32_t a) const {
    check(a, 1, "load");
    return bytes[a];
  }

  uint16_t load16(uint32_t a) const {
    check(a, 2, "load");
    return (uint16_t)(bytes[a] | bytes[a + 1] << 8);
  }

  uint32_t load32(uint32_t a) const {
    check(a, 4, "load");
    return (uint32_t)bytes[a] | (uint32_t)bytes[a + 1] << 8 |
           (uint32_t)bytes[a + 2] << 16 | (uint32_t)bytes[a + 3] << 24;
  }

  void store8(uint32_t a, uint8_t v) {
    check(a, 1, "store");
    bytes[a] = v;
  }

  void store16(uint32_t a, uint16_t v) {
    check(a, 2, "store");
    bytes[a] = (uint8_t)v;
    bytes[a + 1] = (uint8_t)(v >> 8);
  }

  void store32(uint32_t a, uint32_t v) {
    check(a, 4, "store");
    bytes[a] = (uint8_t)v;
    bytes[a + 1] = (uint8_t)(v >> 8);
    bytes[a + 2] = (uint8_t)(v >> 16);
    bytes[a + 3] = (uint8_t)(v >> 24);
  }
};
