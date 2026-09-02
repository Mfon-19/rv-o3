// Functional units.
//
// Execution is a set of explicit units, so that an instruction's
// latency (cycles until its result exists) and a unit's throughput
// (how often it can start a new op) are separate, configurable
// properties:
//
//      N x integer ALU   latency 1, pipelined
//      1 x branch unit   latency 1, pipelined
//      1 x multiplier    latency mulLatency, pipelined (configurable)
//      1 x divider       latency divLatency, non-pipelined (busy throughout)
//      1 x AGU           latency 1; drains to the LSQ, not a WB port
//
// The result VALUE is computed by execute() at issue (semantics stay
// in isa/); a unit only delays the result's visibility. Every unit ends
// in a one-entry output slot holding a completed op that has not yet
// won a writeback port. While that slot is occupied the unit cannot
// finish its next op (a pipelined unit's whole pipe holds, a
// non-pipelined unit stays busy), which is how a shortage of
// writeback ports slows everything upstream of it.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"

// "No physical register": branches, stores, and writes to x0
constexpr uint8_t kNoReg = 0xFF;

// An instruction in flight inside a functional unit
struct FuOp {
  bool valid = false;
  uint64_t seq = 0;    // program order; writeback arbitration picks oldest
  uint32_t robIdx = 0; // this op's reorder-buffer entry
  Instr ins;
  uint32_t pc = 0;
  uint32_t value = 0;    // result / effective address (memory ops)
  uint8_t pdst = kNoReg; // physical destination register
  uint32_t lsqIdx = 0;   // memory ops: the LSQ entry to fill at AGU drain
  bool redirect = false; // branches: the true direction and target,
  uint32_t target = 0;   // computed at issue; writeback compares them
                         // against what fetch predicted
};

// A fixed-latency unit, pipelined or not. Pipelined: a shift register
// with one slot per cycle of latency, accepting a new op every cycle.
// Non-pipelined: one op occupies the unit for its whole latency
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

  // Clock edge: advance one cycle. While the output slot is occupied a
  // pipelined unit freezes entirely (nothing shifts, bubbles included)
  // and a non-pipelined unit keeps its finished result parked inside
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

  // Squash: kill every in-flight op younger than seq. Their results
  // must never write back
  void flushYounger(uint64_t seq) {
    for (FuOp &s : stages)
      if (s.valid && s.seq > seq)
        s = FuOp{};
    if (cur.valid && cur.seq > seq) {
      cur = FuOp{};
      remaining = 0;
    }
    if (out.valid && out.seq > seq)
      out = FuOp{};
  }
};
