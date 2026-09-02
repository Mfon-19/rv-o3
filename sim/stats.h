// Simulation statistics.
//
// The core-side counters, accumulated by the timing model as it runs
// and printed to stderr at the end. Per-cache counters live with the
// caches (CacheStats in memory/cache.h) and per-unit counters with the
// functional units (FuUnit in core/fu.h).

#pragma once

#include <cstdint>

struct Stats {
  uint64_t cycles = 0;
  uint64_t retired = 0; // instructions committed from the ROB head

  // Dispatch-stall cycles by cause (the first blocking resource wins)
  uint64_t dsRobFull = 0, dsIqFull = 0, dsLsqFull = 0, dsNoPreg = 0,
           dsSerialize = 0, dsFetchEmpty = 0;

  uint64_t branches = 0;    // branches committed...
  uint64_t mispredicts = 0; // ...and how many of them were predicted wrong
  uint64_t flushes = 0;     // recoveries (mispredicts plus load replays)
  uint64_t wbConflicts = 0; // finished results that waited a cycle because
                            // every writeback port was already taken
  uint64_t loadsForwarded = 0; // loads served directly by an older store
  uint64_t specLoads = 0;      // loads that ran ahead of an older store
                               // whose address was not known yet
  uint64_t loadReplays = 0;    // such guesses that were wrong: the load
                               // was flushed and re-executed
  uint64_t sbCommitStalls = 0; // cycles commit stalled on a full store buffer
  uint64_t issuedOps = 0;      // total ops issued (for average issue width)
  uint64_t dataStallCycles = 0; // cycles some issued load was waiting on
                                // the cache

  // Per-cycle occupancy sums, divided by cycles at print time
  uint64_t robOccSum = 0, iqOccSum = 0, lsqOccSum = 0, sbOccSum = 0;
  uint64_t pregsUsedSum = 0; // physical registers held by in-flight dests

  // Attribution intervals
  uint64_t recoveryLossCycles = 0;   // recover() -> next successful dispatch
  uint64_t memRetireStallCycles = 0; // commit blocked on an unfinished load
};
