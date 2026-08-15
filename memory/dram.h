// Fixed-latency main memory, the bottom of the hierarchy.
//
// Pipelined: it accepts one request per cycle and each completes
// independently `latency` cycles later — modeling a memory banked
// enough to sustain the miss-level parallelism the caches above can
// generate. With latency 1 it answers combinationally, which is
// exactly the original flat-memory timing — flat mode attaches two of
// these directly to the core's fetch and data ports.
//
// Writes are applied to the backing store at access() and acknowledged
// after the latency. Applying early keeps the backing store current
// the moment a writeback leaves the cache above (which is what lets
// evicted lines be fire-and-forget), and arrival order at this single
// point IS the memory order, so nothing can observe the difference.

#pragma once

#include <deque>
#include <vector>

#include "memory/memory.h"
#include "memory/request.h"

class DRAM : public MemPort {
public:
  DRAM(Memory &backing, uint32_t latency)
      : backing(backing), latency(latency ? latency : 1) {}

  bool canAccept() const override { return !acceptedThisCycle; }
  void access(const MemRequest &req) override;
  bool hasResponse() const override { return !respQ.empty(); }
  MemResponse response() override;
  void tick() override;

private:
  MemResponse perform(const MemRequest &req); // read the backing store

  Memory &backing;
  uint32_t latency;
  bool acceptedThisCycle = false; // bandwidth: one new request per cycle
  struct Txn {
    MemResponse resp;
    uint32_t remaining;
  };
  std::vector<Txn> inflight;
  std::deque<MemResponse> respQ;
};
