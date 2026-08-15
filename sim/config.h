// Simulation configuration.
//
// Every knob a model can be built with lives here. Defaults below are
// the shipped machine; a config file (-C) and command-line overrides
// (-O key=value) can change any of them by name; see the knob table
// in sim/config.cpp, or run rvsim -p to list effective values.

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
  uint64_t maxCycles = 10'000'000; // cycle budget (instructions for -f)
  bool trace = false;              // -t: pipeline trace, one line per cycle
  bool dumpRegs = false;           // -r: register dump at the end
  bool refModel = false;           // -f: run the functional reference model
  bool diffCheck = false;          // -d: run the reference model alongside
                                   // and compare every retired instruction

  // Memory hierarchy. Deliberately not exposed on the CLI; these are
  // the fixed defaults until a proper configuration interface exists
  // (the design-space sweeps will need one). flatMemory reproduces the
  // original dual-ported flat-memory timing exactly and is kept for
  // regression debugging
  bool flatMemory = false;   // no caches, direct fixed-latency ports
  uint32_t flatLatency = 1;  // flat-mode access latency
  uint32_t dramLatency = 30; // DRAM latency in cache mode
  CacheConfig l1i{32 * 1024, 8, 64, 1, 2, 2};
  CacheConfig l1d{32 * 1024, 8, 64, 1, 4, 4};
  CacheConfig l2{256 * 1024, 8, 64, 4, 4, 4};

  // Functional units. Internal like the cache geometry; the config
  // file and -O overrides expose these, not dedicated CLI flags
  uint32_t aluCount = 2;        // 1-cycle pipelined integer ALUs
  uint32_t mulLatency = 3;      // multiplier result latency
  bool mulPipelined = true;     // one new multiply may start per cycle
  uint32_t divLatency = 12;     // divider occupancy (non-pipelined)
  uint32_t wbPorts = 2;         // completions written back per cycle

  // Out-of-order core shape, internal like everything else
  uint32_t width = 2;        // rename/dispatch, issue, and commit width
  uint32_t robSize = 32;
  uint32_t iqSize = 16;      // integer issue queue entries
  uint32_t lsqSize = 16;     // load/store queue entries
  uint32_t sbSize = 8;       // store buffer (committed stores)
  uint32_t physRegs = 0;     // 0 = derive 32 + robSize (validateConfig);
                             // explicit values allow pressure studies
  uint32_t fetchQSize = 8;   // fetched instructions waiting to dispatch
  bool usePredictor = true;  // false: static not-taken (bring-up mode)
  MemOrder memOrder = MemOrder::Speculative; // how far loads may run
                             // ahead of older stores (see enum above)
  bool depPredictor = false; // remember loads that caused replays and
                             // make them wait next time (Speculative
  uint32_t depTableSize = 64; // mode only)
  uint32_t phtBits = 10;     // gshare: 2^10 two-bit counters
  uint32_t ghrBits = 0;      // history bits; 0 = plain bimodal (these
                             // kernels end before history would warm up)
  uint32_t btbEntries = 64;
  uint32_t rasEntries = 8;
};

// The named-knob interface (sim/config.cpp). All errors are fatal;
// a silently ignored typo in a sweep config produces wrong science
void loadConfigFile(SimConfig &c, const char *path);    // key = value lines
void applyConfigOverride(SimConfig &c, const char *kv); // "key=value"
void dumpConfig(const SimConfig &c, FILE *out);
void validateConfig(SimConfig &c); // bounds checks + derived defaults
