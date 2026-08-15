// The request/response vocabulary of the timed memory system.
//
// Every level of the hierarchy (caches, DRAM) implements MemPort. A
// requester calls canAccept() (the backpressure signal), then access()
// to start an access, then polls done() each cycle and consumes the
// result with response(). Everything is blocking: a port holds at most
// one access at a time, and canAccept() stays false until the current
// response has been consumed.
//
// Latency convention: latency 1 means the response is ready in the same
// cycle access() is called (a combinational answer, like the original
// flat memory). Latency L makes the response ready after L-1 further
// tick()s. tick() advances a device by one clock cycle; the simulator
// ticks the whole hierarchy bottom-up once per core cycle.

#pragma once

#include <cstdint>
#include <vector>

struct MemRequest {
  uint32_t addr = 0;
  uint32_t size = 0; // 1, 2, 4, or lineBytes (a whole-line transfer)
  bool isWrite = false;
  uint32_t wdata = 0;          // scalar store data (size <= 4)
  std::vector<uint8_t> wline;  // line-write payload (size == lineBytes)
};

struct MemResponse {
  uint32_t rdata = 0;          // scalar load data, zero-extended
  std::vector<uint8_t> rline;  // line-read payload (size == lineBytes)
};

struct MemPort {
  virtual ~MemPort() = default;
  virtual bool canAccept() const = 0;
  virtual void access(const MemRequest &req) = 0; // requires canAccept()
  virtual bool done() const = 0;                  // response ready?
  virtual MemResponse response() = 0;             // consume; port goes idle
  virtual void tick() = 0;                        // advance one cycle
};
