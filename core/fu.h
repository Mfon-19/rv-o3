// Functional units.
//
// The single EX stage becomes a set of explicit units, so that an
// instruction's latency (cycles until its result exists) and a unit's
// throughput (how often it can start a new op) are separate,
// configurable properties:
//
//      N x integer ALU   latency 1, pipelined
//      1 x branch unit   latency 1, pipelined
//      1 x multiplier    latency 3, pipelined (one new op per cycle)
//      1 x divider       latency 12+, non-pipelined (busy throughout)
//      1 x LSU           address generation + asynchronous cache access
//
// The result VALUE is computed by execute() at issue — semantics stay
// in isa/ — a unit only delays the result's visibility. Every unit ends
// in a one-entry output slot holding a completed op that has not yet
// won a writeback port; an occupied slot backpressures the unit
// (a pipelined unit's whole pipe holds, a non-pipelined unit stays
// busy), which is how limited writeback bandwidth propagates upstream.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/commit.h"
#include "isa/isa.h"
#include "memory/request.h"

// An instruction in flight inside a functional unit
struct FuOp {
  bool valid = false;
  uint64_t seq = 0;    // issue order; writeback arbitration picks the oldest
  uint32_t robIdx = 0; // this op's retire-queue entry
  Instr ins;
  uint32_t pc = 0;
  uint32_t value = 0; // result / effective address (memory ops)
  // Stores only: the data to write. If the producer was still in
  // flight at issue, storeDataReady is false and the writeback
  // broadcast fills the value in before the access starts
  uint32_t storeData = 0;
  bool storeDataReady = true;
  uint8_t storeDataReg = 0;
  std::optional<MemoryWrite> memWrite; // filled when a store performs
};

// A fixed-latency unit, pipelined or not. Pipelined: a shift register
// one slot per cycle of latency, accepting a new op every cycle.
// Non-pipelined: a single op occupies the unit for its whole latency
struct FuUnit {
  const char *name;
  uint32_t latency;
  bool pipelined;
  std::vector<FuOp> stages; // pipelined occupancy
  FuOp cur;                 // non-pipelined occupancy
  uint32_t remaining = 0;
  FuOp out; // completed, waiting for a writeback port
  uint64_t ops = 0, busyCycles = 0;

  FuUnit(const char *name, uint32_t latency, bool pipelined)
      : name(name), latency(latency), pipelined(pipelined) {
    if (pipelined)
      stages.resize(latency);
  }

  bool busy() const {
    if (!pipelined)
      return cur.valid;
    for (const FuOp &s : stages)
      if (s.valid)
        return true;
    return false;
  }

  bool canAccept() const {
    return pipelined ? !stages[0].valid : (!cur.valid && !out.valid);
  }

  void accept(const FuOp &op) {
    ops++;
    if (pipelined)
      stages[0] = op;
    else {
      cur = op;
      remaining = latency;
    }
  }

  // Clock edge: advance one cycle. A blocked output slot freezes a
  // pipelined unit entirely (no internal compression), and keeps a
  // non-pipelined unit's completion parked in the unit
  void tick() {
    if (busy())
      busyCycles++;
    if (pipelined) {
      if (out.valid)
        return;
      out = stages[latency - 1];
      for (uint32_t i = latency - 1; i >= 1; i--)
        stages[i] = stages[i - 1];
      stages[0] = FuOp{};
    } else if (cur.valid && --remaining == 0) {
      out = cur; // canAccept() kept out empty while cur ran
      cur = FuOp{};
    }
  }
};

// The load/store unit: one cycle of address generation (the agu slot),
// then an asynchronous access at the data port (the acc slot). Only
// dependents and younger memory ops wait on a slow access now — the
// rest of the core keeps issuing, where the five-stage pipeline froze
// entirely
struct LSU {
  MemPort *dmem = nullptr;
  FuOp agu, acc, out;
  bool outstanding = false; // access() issued, response not consumed
  bool ready = false;       // acc finished, waiting for the output slot
  uint64_t ops = 0, busyCycles = 0;
  uint64_t dataStallCycles = 0; // cycles waiting on port or store data

  bool busy() const { return agu.valid || acc.valid; }
  bool canAccept() const { return !agu.valid; }
  void accept(const FuOp &op) {
    ops++;
    agu = op;
  }

  // Advance one cycle: finish an outstanding access, drain to the
  // output slot, move the next op into the access slot, start it
  void operate(std::vector<std::string> *events);

  // Writeback broadcast: an armed store still waiting for its data
  // captures the value the moment the producer writes back
  void capture(uint8_t rd, uint32_t value);

private:
  void finalize(const MemResponse &resp);
  void startAccess(std::vector<std::string> *events);
};
