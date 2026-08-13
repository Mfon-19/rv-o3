// Simulation configuration, filled in from the command line.
//
// Every knob a model can be built with lives here, so that future
// configuration sweeps (cache sizes, ROB sizes, widths...) have one
// place to plug into.

#pragma once

#include <cstddef>
#include <cstdint>

struct CacheConfig {
  uint32_t sizeBytes;
  uint32_t ways;
  uint32_t lineBytes;
  uint32_t hitLatency; // cycles; 1 = same-cycle
};

struct SimConfig {
  size_t memBytes = 1u << 20;      // 1 MiB
  uint64_t maxCycles = 10'000'000; // cycle budget (instructions for -f)
  bool trace = false;              // -t: per-cycle pipeline trace
  bool dumpRegs = false;           // -r: register dump at the end
  bool refModel = false;           // -f: run the functional reference model
  bool diffCheck = false;          // -d: lockstep pipeline-vs-reference check

  // Memory hierarchy (phase 1). Deliberately not exposed on the CLI —
  // these are the fixed defaults until a proper configuration interface
  // exists (phase 5's sweeps will need one). flatMemory reproduces the
  // phase-0 dual-ported flat-memory timing exactly and is kept for
  // regression debugging
  bool flatMemory = false;   // no caches, direct fixed-latency ports
  uint32_t flatLatency = 1;  // flat-mode access latency
  uint32_t dramLatency = 30; // DRAM latency in cache mode
  CacheConfig l1i{32 * 1024, 8, 64, 1};
  CacheConfig l1d{32 * 1024, 8, 64, 1};
  CacheConfig l2{256 * 1024, 8, 64, 4};
};
