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
//      N x AGU           latency 1; drains to the LSQ, not a WB port
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

// A result in flight. The ROB owns the decoded instruction and PC until
// retirement; units carry only its index and execution result.
struct FuOp {
  uint64_t seq = 0;    // program order; writeback arbitration picks oldest
  uint32_t robIdx = 0; // this op's reorder-buffer entry
  uint32_t value = 0;   // result / effective address (memory ops)
  uint32_t lsqIdx = 0;  // memory ops: the LSQ entry to fill at AGU drain
  uint32_t target = 0;  // branch target, checked against fetch's prediction
  uint8_t pdst = kNoReg; // physical destination register
  bool valid = false;
  bool redirect = false; // branches: the true direction
};

// A fixed-latency unit, pipelined or not. Pipelined: a circular buffer
// with one slot per cycle of latency, accepting a new op every cycle.
// Non-pipelined: one op occupies the unit for its whole latency
struct FuUnit {
  const char *name;
  uint32_t latency;
  bool pipelined;
  std::vector<FuOp> stages; // circular pipeline, indexed by inputSlot
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
    return pipelined ? inFlight != 0 : cur.valid;
  }

  bool canAccept() const {
    return pipelined ? !stages[inputSlot].valid : (!cur.valid && !out.valid);
  }

  void accept(const FuOp &op) {
    ops++;
    if (pipelined) {
      stages[inputSlot] = op;
      inFlight++;
    } else {
      cur = op;
      remaining = latency;
    }
  }

  // Clock edge: advance one cycle. While the output slot is occupied a
  // pipelined unit freezes entirely (nothing shifts, bubbles included)
  // and a non-pipelined unit keeps its finished result parked inside.
  // Returns true exactly when a new output becomes ready for writeback
  bool tick() {
    if (!busy())
      return false;
    busyCycles++;
    if (pipelined) {
      if (out.valid)
        return false;
      // The last stage becomes the next input slot. Advancing the index
      // moves every op one logical stage without copying the records.
      inputSlot = inputSlot ? inputSlot - 1 : latency - 1;
      FuOp &last = stages[inputSlot];
      if (last.valid) {
        out = last;
        last.valid = false;
        inFlight--;
        return true;
      }
    } else if (--remaining == 0) {
      out = cur; // canAccept() kept out empty while cur ran
      cur.valid = false;
      return true;
    }
    return false;
  }

  // Squash: kill every in-flight op younger than seq. Their results
  // must never write back
  void flushYounger(uint64_t seq) {
    for (FuOp &s : stages)
      if (s.valid && s.seq > seq) {
        s.valid = false;
        inFlight--;
      }
    if (cur.valid && cur.seq > seq) {
      cur.valid = false;
      remaining = 0;
    }
    if (out.valid && out.seq > seq)
      out.valid = false;
  }

private:
  uint32_t inputSlot = 0;
  uint32_t inFlight = 0; // pipeline entries; excludes the output slot
};
