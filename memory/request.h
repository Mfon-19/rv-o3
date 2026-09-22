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
// transaction; poll hasResponse() and pop completions with response().
// A requester whose transaction was squashed simply drops the
// completion when its tag no longer matches anything live.
//
// Latency convention: latency 1 means the response is available in the
// same cycle access() is called (a combinational answer). tick()
// advances a device one clock cycle; the simulator ticks the whole
// hierarchy bottom-up once per core cycle.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

// All supported instruction-fetch blocks fit inline. Larger cache refills
// retain owning storage, so queued responses never refer into mutable caches.
class ReadPayload {
public:
  ReadPayload() = default;
  ReadPayload(const ReadPayload &other) {
    assign(other.data(), other.data() + other.size());
  }
  ReadPayload &operator=(const ReadPayload &other) {
    if (this != &other)
      assign(other.data(), other.data() + other.size());
    return *this;
  }
  ReadPayload(ReadPayload &&other) noexcept { moveFrom(std::move(other)); }
  ReadPayload &operator=(ReadPayload &&other) noexcept {
    if (this != &other)
      moveFrom(std::move(other));
    return *this;
  }

  void assign(const uint8_t *first, const uint8_t *last) {
    const size_t size = (size_t)(last - first);
    if (size <= small.size()) {
      if (size)
        std::memcpy(small.data(), first, size);
      large.reset();
    } else {
      if (size != length || !large)
        large.reset(new uint8_t[size]);
      std::memcpy(large.get(), first, size);
    }
    length = size;
  }
  size_t size() const { return length; }
  const uint8_t *data() const {
    return length <= small.size() ? small.data() : large.get();
  }
  uint8_t operator[](size_t i) const { return data()[i]; }

private:
  // Unused inline bytes are neither initialized nor copied.
  std::array<uint8_t, 32> small;
  std::unique_ptr<uint8_t[]> large;
  size_t length = 0;

  void moveFrom(ReadPayload &&other) {
    length = other.length;
    if (length && length <= small.size())
      std::memcpy(small.data(), other.small.data(), length);
    large = std::move(other.large);
    other.length = 0;
  }
};

struct MemRequest {
  std::vector<uint8_t> wline; // line-write payload (size == lineBytes)
  uint64_t tag = 0;          // requester transaction id, echoed back
  uint32_t addr = 0;
  uint32_t size = 0;         // 1, 2, 4 (scalar), the fetch block, or lineBytes
  uint32_t wdata = 0;        // scalar store data (size <= 4)
  uint8_t src = 0;           // requester port id, echoed back
  bool isWrite = false;
};

struct MemResponse {
  ReadPayload rline;         // multi-word payload (size > 4 reads)
  uint64_t tag = 0;
  uint32_t rdata = 0;         // scalar load data, zero-extended
  uint8_t src = 0;
};

struct MemPort {
  virtual ~MemPort() = default;
  virtual bool canAccept() const = 0;
  virtual void access(const MemRequest &req) = 0; // requires canAccept()
  virtual bool hasResponse() const = 0; // any completion ready?
  virtual MemResponse response() = 0;   // pop one completion
  virtual void tick() = 0;              // advance one cycle
};
