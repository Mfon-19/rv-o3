// Nonblocking set-associative write-back cache with MSHRs (hit-under-miss and
// miss merging), LRU replacement, and a writeback queue for dirty victims.

#pragma once

#include <cstdint>
#include <vector>

#include "memory/memory.h"
#include "sim/config.h"

struct CacheStats {
  uint64_t accesses = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t mergedMisses = 0;
  uint64_t hitUnderMiss = 0;
  uint64_t dirtyEvictions = 0;
  uint64_t latencySum = 0;
};

class Cache final : public MemPort {
public:
  Cache(const char *name, const CacheConfig &config, MemPort *lowerLevel,
        uint8_t srcId);

  // Backpressure when all MSHRs are occupied or the writeback queue is full.
  bool canAccept() const override {
    return pendingMisses < mshrs.size() && writebackQueue.size() < config.wbq;
  }

  void access(const MemRequest &req) override;

  // Fast scalar L1D access avoiding 128-byte MemRequest line setup on hits.
  void accessWord(uint32_t addr, uint32_t size, bool isWrite, uint32_t writeData,
                  uint64_t tag);

  // Single-cycle direct hit fast paths bypassing responseQueue on L1D/L1I hits.
  bool tryAccessWordImmediate(uint32_t addr, uint32_t size, bool isWrite,
                              uint32_t writeData, uint32_t &outReadData);
  const uint8_t *tryFetchLineImmediate(uint32_t addr);
  bool hasResponse() const override { return !responseQueue.empty(); }
  const MemResponse &frontResponse() const override { return responseQueue.front(); }
  void popResponse() override { responseQueue.popFront(); }
  void tick() override;

  // Delivers a line refill returned by the lower memory level.
  void deliverBelowResponse(const MemResponse &response);

  // Side-channel read for syscalls and differential checking without timing effects.
  bool peek8(uint32_t addr, uint8_t &outByte) const;

  const char *name;
  CacheStats stats;

private:
  struct WaitingRequest {
    MemRequest req;
    uint64_t issueTick = 0;
  };

  struct Mshr {
    bool valid = false;
    uint32_t lineAddr = 0;
    bool refillSent = false;
    std::vector<WaitingRequest> waiting;
  };

  struct WritebackEntry {
    uint32_t addr = 0;
    Line line;
  };

  struct PendingHit {
    MemResponse response;
    uint64_t readyAt = 0;
  };

  // Refill that arrived while the writeback queue was full and could not evict.
  struct PendingInstall {
    uint32_t mshrIndex = 0;
    Line line;
  };

  uint32_t setOf(uint32_t addr) const { return (addr >> lineShift) & (setCount - 1); }
  uint32_t tagOf(uint32_t addr) const { return addr >> tagShift; }
  uint32_t offsetOf(uint32_t addr) const { return addr & (config.lineBytes - 1); }

  uint32_t lineAddrOf(uint32_t set, uint32_t way) const {
    return (tags[set * config.ways + way] << tagShift) | (set << lineShift);
  }

  uint8_t *lineData(uint32_t set, uint32_t way) {
    return data.data() + (size_t)(set * config.ways + way) * config.lineBytes;
  }

  const uint8_t *lineData(uint32_t set, uint32_t way) const {
    return data.data() + (size_t)(set * config.ways + way) * config.lineBytes;
  }

  int findWay(uint32_t set, uint32_t tag) const;
  uint32_t victimWay(uint32_t set) const;
  int mshrFor(uint32_t lineAddr) const;
  bool mergeIntoMshr(uint32_t lineAddr, const MemRequest &req);
  void performOnLine(const MemRequest &req, uint32_t set, uint32_t way, MemResponse &response);
  bool installLine(uint32_t lineAddr, const uint8_t *srcData, bool isDirty, uint32_t &outSet, uint32_t &outWay);
  bool tryInstall(uint32_t mshrIndex, const Line &line);
  MemResponse &reserveHit();

  static constexpr uint32_t kInvalidTag = UINT32_MAX;

  CacheConfig config;
  uint32_t setCount = 0;
  unsigned lineShift = 0, tagShift = 0;
  MemPort *lowerLevel = nullptr;
  uint8_t srcId = 0;

  std::vector<uint8_t> data;
  std::vector<uint32_t> tags;
  std::vector<uint8_t> dirty;
  std::vector<uint64_t> lru;
  uint64_t lruTick = 0;

  std::vector<Mshr> mshrs;
  uint32_t pendingMisses = 0;
  FastQueue<WritebackEntry> writebackQueue;
  FastQueue<PendingInstall> pendingInstalls;
  FastQueue<PendingHit> hitPipeline;
  FastQueue<MemResponse> responseQueue;
  uint64_t tickCount = 0;
};
