// Simulation statistics.
//
// Counters are accumulated by the timing model as it runs and printed
// to stderr at the end. Program output goes to stdout; trace,
// statistics and diagnostics go to stderr. Per-cache counters live with
// the caches themselves (CacheStats in memory/cache.h) and per-unit
// counters with the functional units (FuUnit in core/fu.h); this
// struct holds the core-side counters.

#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>

struct Stats {
  uint64_t cycles = 0;
  uint64_t retired = 0; // instructions committed from the ROB head

  // Dispatch-stall cycles by cause:
  uint64_t dsRobFull = 0, dsIqFull = 0, dsLsqFull = 0, dsNoPreg = 0,
           dsSerialize = 0, dsFetchEmpty = 0;

  uint64_t branches = 0, mispredicts = 0; // committed / of those, wrong
  uint64_t flushes = 0;                   // recovery events
  uint64_t wbConflicts = 0; // completions held a cycle by port arbitration
  uint64_t loadsForwarded = 0;   // store->load forwards
  uint64_t specLoads = 0;        // loads issued past unknown store addrs
  uint64_t loadReplays = 0;      // ordering violations, flush-and-refetch
  uint64_t sbCommitStalls = 0;   // store-buffer-full commit stalls
  uint64_t issuedOps = 0;        // total ops issued (avg issue width)
  uint64_t fetchStallCycles = 0; // cycles dispatch had nothing fetched
  uint64_t dataStallCycles = 0;  // cycles the LSQ waited on the data port

  // Per-cycle occupancy sums, divided by cycles at print time
  uint64_t robOccSum = 0, iqOccSum = 0, lsqOccSum = 0, sbOccSum = 0;
  uint64_t pregsUsedSum = 0; // physical registers held by in-flight dests
  // Attribution (definitions in doc/phase-5-validation.md):
  uint64_t recoveryLossCycles = 0;    // recover() -> next successful dispatch
  uint64_t memRetireStallCycles = 0;  // commit blocked on an unfinished load

  void printMemStalls() const {
    fprintf(stderr, "--- rvsim: %" PRIu64 " ifetch stall cycles, %" PRIu64
            " data stall cycles\n",
            fetchStallCycles, dataStallCycles);
  }

  void printExit(int exitCode) const {
    fprintf(stderr, "--- rvsim: exit code %d\n", exitCode);
  }
};
