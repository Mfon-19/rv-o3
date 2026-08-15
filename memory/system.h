// The assembled memory system: backing store, topology, and the two
// top-level ports the core talks to.
//
// Cache mode (default):
//
//   core IF port  -> L1I --+
//                          +--> unified L2 --> DRAM --> backing Memory
//   core MEM port -> L1D --+
//
// Flat mode (SimConfig::flatMemory): two independent fixed-latency
// ports straight to the backing store, which at latency 1 reproduces
// the original timing model exactly (dual-ported flat memory, no
// structural hazards).
//
// Every level is nonblocking and tagged; the MemorySystem's tick
// routes each level's completions to its requester: DRAM completions
// go to the L2, and the L2's completions, which carry the src id of
// the L1 that asked, go back to the right L1. Backpressure is MSHR
// and writeback-queue occupancy (canAccept), not a busy port.

#pragma once

#include <optional>

#include "memory/cache.h"
#include "memory/dram.h"
#include "memory/memory.h"
#include "memory/request.h"
#include "sim/config.h"

// Requester ids carried in MemRequest::src for response routing
enum : uint8_t { SRC_L1I = 0, SRC_L1D = 1, SRC_L2 = 2 };

struct MemorySystem {
  explicit MemorySystem(const SimConfig &cfg);

  void tick(); // advance the whole hierarchy one cycle, bottom-up

  // Functional byte read for the simulator's own use (syscall 3):
  // newest copy wins: L1D, then L2, then the backing store
  uint8_t peek8(uint32_t addr) const;

  void printStats(uint64_t retired) const;

  Memory backing;
  bool flat;
  DRAM dram;                 // cache mode: below L2; flat mode: data port
  std::optional<DRAM> flatI; // flat mode only: independent fetch port
  std::optional<Cache> l2, l1i, l1d;
  MemPort *imem; // what the core's fetch talks to
  MemPort *dmem; // what the core's MEM stage talks to
};
