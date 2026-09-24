// Simulation configuration.
//
// Every knob a machine can be built with lives here. The defaults are
// the shipped machine; a config file (-C) and command-line overrides
// (-O key=value) can change any of them by name. See the knob table in
// sim/config.cpp, or run rvsim -p to list effective values.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct CacheConfig {
  uint32_t sizeBytes;
  uint32_t ways;
  uint32_t lineBytes;
  uint32_t hitLatency; // cycles; 1 = same-cycle
  uint32_t mshrs;      // outstanding missed lines (nonblocking depth)
  uint32_t wbq;        // dirty victims parked awaiting writeback
};

// How aggressively loads move around older stores
enum class MemOrder {
  Conservative, // wait until every older store address is known
  Bypass,       // pass resolved, provably non-overlapping stores
  Speculative,  // pass unknown addresses too; replay on a violation
};

struct SimConfig {
  size_t memBytes = 1u << 20;      // 1 MiB
  uint64_t maxCycles = 10'000'000; // cycle budget
  bool trace = false;              // -t: pipeline trace, one line per cycle
  bool dumpRegs = false;           // -r: register dump at the end
  bool diffCheck = false;          // -d: run the reference model alongside
                                   // and compare every retired instruction

  // Memory hierarchy. flatMemory drops the caches for two independent
  // fixed-latency ports, which isolates core timing from memory timing
  bool flatMemory = false;   // no caches, direct fixed-latency ports
  uint32_t flatLatency = 1;  // flat-mode access latency
  uint32_t dramLatency = 30; // DRAM latency in cache mode
  CacheConfig l1i{32 * 1024, 8, 64, 1, 2, 2};
  CacheConfig l1d{32 * 1024, 8, 64, 1, 4, 4};
  CacheConfig l2{256 * 1024, 8, 64, 4, 4, 4};

  // Functional units
  uint32_t aluCount = 2;    // 1-cycle pipelined integer ALUs
  uint32_t aguCount = 1;    // 1-cycle pipelined address-generation units
  uint32_t dataPorts = 1;   // total new core load/store accesses per cycle
  uint32_t mulLatency = 3;  // multiplier result latency
  bool mulPipelined = true; // one new multiply may start per cycle
  uint32_t divLatency = 12; // divider occupancy (non-pipelined)
  uint32_t wbPorts = 2;     // completions written back per cycle

  // Out-of-order core shape
  uint32_t width = 2;         // rename/dispatch, issue, and commit width
  uint32_t robSize = 32;
  uint32_t iqSize = 16;       // issue queue entries
  uint32_t lsqSize = 16;      // load/store queue entries
  uint32_t sbSize = 8;        // store buffer (committed stores)
  uint32_t physRegs = 0;      // 0 = derive 32 + robSize (validateConfig);
                              // explicit values allow pressure studies
  uint32_t fetchQSize = 8;    // fetched instructions waiting to dispatch
  uint32_t fetchBytes = 0;    // 0 = derive from width; otherwise 8/16/32/64
  bool usePredictor = true;   // false: static not-taken (bring-up mode)
  MemOrder memOrder = MemOrder::Speculative; // see the enum above
  bool depPredictor = false;  // Speculative mode: loads that replayed
                              // recently wait for unknown store addresses
  uint32_t depTableSize = 64; // entries in that PC-indexed table
  uint32_t phtBits = 10;      // gshare: 2^phtBits two-bit counters
  uint32_t ghrBits = 0;       // history bits; 0 = plain bimodal
  uint32_t btbEntries = 64;
  uint32_t btbWays = 1;       // 1 = direct-mapped; more ways use LRU
  uint32_t rasEntries = 8;
  bool rasRepair = false;     // restore the RAS top after a flush
  // TAGE direction prediction (core/predictor.h): 0 tagged tables keeps
  // gshare. With TAGE, phtBits sizes the bimodal base and ghrBits is unused
  uint32_t tageTables = 0;
  uint32_t tageTableBits = 10; // 2^bits entries per tagged table
  uint32_t tageTagBits = 11;
  uint32_t tageMinHist = 4;    // history lengths grow geometrically
  uint32_t tageMaxHist = 128;  // from the shortest to the longest table

  // Cache fetches use aligned power-of-two blocks. Intermediate core
  // widths share the next larger fetch block (5..8 use 32 bytes).
  uint32_t fetchBlockBytes() const {
    if (fetchBytes)
      return fetchBytes;
    return width <= 2 ? 8 : width <= 4 ? 16 : 32;
  }
};

// The named-knob interface (sim/config.cpp). All errors are fatal: a
// silently ignored typo in a sweep config produces wrong results that
// look right
void loadConfigFile(SimConfig &c, const char *path);    // key = value lines
void applyConfigOverride(SimConfig &c, const char *kv); // "key=value"
void dumpConfig(const SimConfig &c, FILE *out);
void validateConfig(SimConfig &c); // derived defaults + bounds checks
