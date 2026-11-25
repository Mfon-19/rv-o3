// Load/Store Queue (LSQ) for in-flight memory ordering and speculative load
// tracking, plus the post-commit Store Buffer that drains committed stores.

#pragma once

#include <cstdint>
#include <vector>

#include "core/ring.h"
#include "isa/isa.h"

struct LsqEntry {
  uint64_t seq = 0;
  uint32_t robIdx = 0;
  uint32_t pc = 0;
  uint32_t addr = 0;
  uint32_t data = 0;
  uint32_t value = 0;
  uint16_t generation = 0; // Distinguishes reused slots for late dmem responses.
  Op op = Op::ILLEGAL;
  uint8_t destPhysReg = 0xFF;
  uint8_t size = 4;
  uint8_t dataPhysReg = 0xFF;
  bool isStore = false;
  bool addrValid = false;
  bool dataReady = true;
  bool done = false;
  bool issued = false;
  bool reported = false;
};

inline bool overlaps(uint32_t addrA, uint32_t sizeA, uint32_t addrB,
                     uint32_t sizeB) {
  return addrA + sizeA > addrB && addrB + sizeB > addrA;
}

class LSQ : public Ring<LsqEntry> {
public:
  explicit LSQ(uint32_t capacity) : Ring(capacity), generationCounter(capacity, 0) {}

  uint32_t alloc() {
    const uint32_t idx = Ring::alloc();
    at(idx).generation = ++generationCounter[idx];
    return idx;
  }

  // Returns the sequence number of the oldest store whose address is still unknown.
  uint64_t oldestUnknownStore() {
    for (uint32_t i = 0; i < count(); i++) {
      const LsqEntry &entry = nth(i);
      if (entry.isStore && !entry.addrValid)
        return entry.seq;
    }
    return UINT64_MAX;
  }

  // Returns the sequence number of the oldest load currently waiting on dmem.
  uint64_t oldestPendingLoad() {
    for (uint32_t i = 0; i < count(); i++) {
      const LsqEntry &entry = nth(i);
      if (!entry.isStore && entry.issued && !entry.done)
        return entry.seq;
    }
    return UINT64_MAX;
  }

  bool hasPendingLoads() { return oldestPendingLoad() != UINT64_MAX; }

private:
  std::vector<uint16_t> generationCounter;
};

struct StoreBufferEntry {
  uint32_t addr = 0;
  uint8_t size = 4;
  uint32_t data = 0;
  bool inFlight = false;
  bool acked = false;
  uint32_t txnId = 0;
};

// FIFO of committed stores waiting to be sent to L1D and acknowledged.
class StoreBuffer : public Ring<StoreBufferEntry> {
public:
  explicit StoreBuffer(uint32_t capacity) : Ring(capacity) {}

  StoreBufferEntry *nextToIssue() {
    return issuedCount < count() ? &nth(issuedCount) : nullptr;
  }

  uint32_t nextIssueSlot() const { return indexOf(issuedCount); }
  void issue() { nth(issuedCount++).inFlight = true; }

  void popHead() {
    --issuedCount;
    Ring::popHead();
  }

private:
  uint32_t issuedCount = 0;
};
