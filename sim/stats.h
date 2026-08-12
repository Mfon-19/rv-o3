// Simulation statistics.
//
// Counters are accumulated by the timing model as it runs and printed
// to stderr at the end. Program output goes to stdout; trace,
// statistics and diagnostics go to stderr.

#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>

struct Stats {
  uint64_t cycles = 0;
  uint64_t retired = 0;       // instructions that completed WB
  uint64_t loadUseStalls = 0; // bubbles inserted by the interlock
  uint64_t redirects = 0;     // taken branches/jumps
  uint64_t squashed = 0;      // wrong-path instructions killed by redirects

  void print(int exitCode) const {
    fprintf(stderr,
            "--- rvsim: %" PRIu64 " cycles, %" PRIu64
            " instructions retired, CPI = %.3f\n",
            cycles, retired, retired ? (double)cycles / (double)retired : 0.0);
    fprintf(stderr,
            "--- rvsim: %" PRIu64 " load-use stalls, %" PRIu64
            " taken branches/jumps (%" PRIu64 " squashed instructions)\n",
            loadUseStalls, redirects, squashed);
    fprintf(stderr, "--- rvsim: exit code %d\n", exitCode);
  }
};
