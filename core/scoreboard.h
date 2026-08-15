// The scoreboard: which architectural registers have a write in flight.
//
// A bit is set when an instruction that writes the register issues and
// cleared when its result is written back. Issue stalls on a set bit
// for a source (RAW: the value does not exist yet) and for the
// destination (WAW: two in-flight writers to one register could
// complete out of order). The WAW rule guarantees at most one in-flight
// writer per register, which is what makes clearing at writeback
// unconditional and the writeback broadcast unambiguous.

#pragma once

#include <cstdint>

struct Scoreboard {
  bool pending[32] = {};

  bool busy(uint8_t r) const { return pending[r]; }
  void set(uint8_t r) {
    if (r != 0) // x0 has no writers
      pending[r] = true;
  }
  void clear(uint8_t r) { pending[r] = false; }
};
