// Simulation statistics.
//
// Counters are accumulated by the timing model as it runs and printed
// to stderr at the end. Program output goes to stdout; trace,
// statistics and diagnostics go to stderr. Per-cache counters live with
// the caches themselves (CacheStats in memory/cache.h) and per-unit
// counters with the functional units (FuUnit/LSU in core/fu.h); this
// struct holds the core-side counters.

#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>

struct Stats {
  uint64_t cycles = 0;
  uint64_t retired = 0; // instructions that retired from the queue head

  // Issue-stall cycles by cause: the issue stage held a decoded
  // instruction back this cycle because...
  uint64_t rawStalls = 0;       // ...a source register was in flight
  uint64_t wawStalls = 0;       // ...its destination was in flight
  uint64_t structStalls = 0;    // ...its functional unit was busy
  uint64_t robFullStalls = 0;   // ...the retire queue was full
  uint64_t serializeStalls = 0; // ...a system op waited for a drain

  uint64_t wbConflicts = 0; // completions held a cycle by port arbitration
  uint64_t redirects = 0;   // taken branches/jumps
  uint64_t squashed = 0;    // wrong-path instructions killed by redirects
  uint64_t fetchStallCycles = 0; // cycles issue had no instruction at all
  uint64_t dataStallCycles = 0;  // cycles the LSU waited on the data port

  void printCore() const {
    fprintf(stderr, "--- rvsim: %" PRIu64 " cycles, %" PRIu64
            " instructions retired, CPI = %.3f\n",
            cycles, retired, retired ? (double)cycles / (double)retired : 0.0);
    fprintf(stderr, "--- rvsim: issue stalls: raw %" PRIu64 ", waw %" PRIu64
            ", structural %" PRIu64 ", rob-full %" PRIu64 ", serialize %"
            PRIu64 "; wb-port holds %" PRIu64 "\n",
            rawStalls, wawStalls, structStalls, robFullStalls,
            serializeStalls, wbConflicts);
    fprintf(stderr, "--- rvsim: %" PRIu64 " taken branches/jumps (%" PRIu64
            " squashed instructions)\n",
            redirects, squashed);
  }

  void printMemStalls() const {
    fprintf(stderr, "--- rvsim: %" PRIu64 " ifetch stall cycles, %" PRIu64
            " data stall cycles\n",
            fetchStallCycles, dataStallCycles);
  }

  void printExit(int exitCode) const {
    fprintf(stderr, "--- rvsim: exit code %d\n", exitCode);
  }
};
