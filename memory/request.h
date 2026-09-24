// The request/response vocabulary of the timed memory system.
//
// Every level of the hierarchy (caches, DRAM) implements MemPort. The
// interface is nonblocking and TAGGED: a requester may have several
// transactions in flight, and completions can come back in any order
// (a hit overtakes an older miss). Each request carries a `tag` chosen
// by the requester and echoed verbatim in the response, so completions
// can be matched to whoever asked, and a `src` port id so a shared
// lower level (the L2 under both L1s) can route responses back to the
// right upper cache.
//
// Discipline: check canAccept(), then access() to start a
// transaction; poll hasResponse() and consume frontResponse() before popResponse().
// A requester whose transaction was squashed simply drops the
// completion when its tag no longer matches anything live.
//
// Latency convention: latency 1 means the response is available in the
// same cycle access() is called (a combinational answer). tick()
// advances a device one clock cycle; the simulator ticks the whole
// hierarchy bottom-up once per core cycle.

#pragma once

#include <array>
#include <cstdint>

// Line-sized payloads travel by value; validateConfig caps line sizes here
constexpr uint32_t kMaxLineBytes = 128;
using Line = std::array<uint8_t, kMaxLineBytes>;

struct MemRequest {
  Line wline;                // line-write payload (size == lineBytes)
  uint64_t tag = 0;          // requester transaction id, echoed back
  uint32_t addr = 0;
  uint32_t size = 0;         // 1, 2, 4 (scalar), the fetch block, or lineBytes
  uint32_t wdata = 0;        // scalar store data (size <= 4)
  uint8_t src = 0;           // requester port id, echoed back
  bool isWrite = false;
};

struct MemResponse {
  Line rline;                // multi-byte read payload (size > 4)
  uint64_t tag = 0;
  uint32_t rdata = 0;         // scalar load data, zero-extended
  uint8_t src = 0;
};

struct MemPort {
  virtual ~MemPort() = default;
  virtual bool canAccept() const = 0;
  virtual void access(const MemRequest &req) = 0; // requires canAccept()
  virtual bool hasResponse() const = 0; // any completion ready?
  // The front stays owned by the port until popResponse(); do not mutate
  // this port while using the reference.
  virtual const MemResponse &frontResponse() const = 0;
  virtual void popResponse() = 0;
  virtual void tick() = 0;              // advance one cycle
};
