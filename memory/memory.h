// Main memory: flat, byte-addressable, little-endian, zero-initialized.
//
// A single unified memory that holds both instructions and data. It
// sits at the bottom of the timed memory system (request/response
// ports, caches, DRAM) as the backing store, and is what the
// functional reference model executes against directly.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Memory {
  std::vector<uint8_t> bytes;

  explicit Memory(size_t size) : bytes(size, 0) {}

  // The two fatal access errors, shared by both models so their
  // messages cannot drift apart. There is no trap handler: an
  // out-of-range access (an access fault in hardware) or a misaligned
  // one (which RV32I permits trapping on) simply ends the run
  [[noreturn]] void failOutOfRange(uint32_t addr, const char *what) const {
    fprintf(stderr, "fatal: %s at 0x%08x is outside memory (%zu bytes)\n",
            what, addr, bytes.size());
    exit(1);
  }
  [[noreturn]] static void failMisaligned(uint32_t addr, const char *what,
                                          uint32_t pc) {
    fprintf(stderr, "fatal: misaligned %s of 0x%08x at pc=0x%08x\n", what,
            addr, pc);
    exit(1);
  }
  void check(uint32_t addr, uint32_t len, const char *what) const {
    if ((uint64_t)addr + len > bytes.size())
      failOutOfRange(addr, what);
  }

  // Little-endian scalar access of 1, 2, or 4 bytes
  uint32_t load(uint32_t a, uint32_t size) const {
    check(a, size, "load");
    uint32_t v = 0;
    for (uint32_t b = 0; b < size; b++)
      v |= (uint32_t)bytes[a + b] << (8 * b);
    return v;
  }
  void store(uint32_t a, uint32_t size, uint32_t v) {
    check(a, size, "store");
    for (uint32_t b = 0; b < size; b++)
      bytes[a + b] = (uint8_t)(v >> (8 * b));
  }
  uint8_t load8(uint32_t a) const { return (uint8_t)load(a, 1); }
  uint32_t load32(uint32_t a) const { return load(a, 4); }

  // Program images land at address 0
  void loadWords(const std::vector<uint32_t> &words) {
    for (size_t i = 0; i < words.size(); i++)
      store((uint32_t)(i * 4), 4, words[i]);
  }
  void loadBytes(const std::vector<uint8_t> &image) {
    check(0, (uint32_t)image.size(), "program image");
    memcpy(bytes.data(), image.data(), image.size());
  }
};
