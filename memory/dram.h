// Fixed-latency main memory, the bottom of the hierarchy.
//
// Wraps the flat backing Memory behind a MemPort: any request (scalar
// or whole-line) is answered after a fixed number of cycles. With
// latency 1 it answers combinationally, which is exactly the original
// flat-memory timing — flat mode attaches two of these directly to
// the core's fetch and data ports.

#pragma once

#include "memory/memory.h"
#include "memory/request.h"

class DRAM : public MemPort {
public:
  DRAM(Memory &backing, uint32_t latency)
      : backing(backing), latency(latency ? latency : 1) {}

  bool canAccept() const override { return !busy; }
  void access(const MemRequest &req) override;
  bool done() const override { return ready; }
  MemResponse response() override;
  void tick() override;

private:
  void complete(); // perform the access on the backing store

  Memory &backing;
  uint32_t latency;
  bool busy = false;
  bool ready = false;
  uint32_t ctr = 0;
  MemRequest cur;
  MemResponse resp;
};
