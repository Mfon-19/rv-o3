// Top-level memory hierarchy wiring split L1I/L1D caches, a unified L2 cache,
// and DRAM (or flat fixed-latency memory when flatMemory is enabled).

#pragma once

#include <optional>

#include "memory/cache.h"
#include "memory/memory.h"
#include "sim/config.h"

// Identifies which upper cache originated a request to the shared L2.
enum : uint8_t { SRC_L1I = 0, SRC_L1D = 1, SRC_L2 = 2 };

struct MemorySystem {
  explicit MemorySystem(const SimConfig &config);

  void tick();

  // Coherent side-channel byte read checking L1D -> L2 -> backing DRAM.
  uint8_t peek8(uint32_t addr) const;

  void printStats(uint64_t retired) const;

  Memory backing;
  bool flat;
  DRAM dram;
  std::optional<DRAM> flatInstrMem;
  std::optional<Cache> l2, l1i, l1d;
  MemPort *imem = nullptr;
  MemPort *dmem = nullptr;
};
