// Simulation configuration, filled in from the command line.
//
// Every knob a model can be built with lives here, so that future
// configuration sweeps (cache sizes, ROB sizes, widths...) have one
// place to plug into.

#pragma once

#include <cstddef>
#include <cstdint>

struct SimConfig {
  size_t memBytes = 1u << 20;      // 1 MiB
  uint64_t maxCycles = 10'000'000; // cycle budget
  bool trace = false;              // -t: per-cycle pipeline trace
  bool dumpRegs = false;           // -r: register dump at the end
};
