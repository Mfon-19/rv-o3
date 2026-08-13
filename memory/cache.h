// A blocking, write-back, write-allocate set-associative cache.
//
// One class serves as L1I, L1D, and L2 — an L1's `below` port is the
// L2, the L2's is DRAM. The phase-1 simplifications, on purpose:
//
//   - blocking: exactly one access in flight, no MSHRs, no
//     hit-under-miss; canAccept() is the backpressure
//   - one victim writeback and one refill at a time, each as a single
//     whole-line transfer to the level below
//   - LRU replacement within a set
//   - a full-line write (an upper cache's dirty writeback landing here)
//     that misses is installed directly without a refill, since every
//     byte of the line is being overwritten
//
// Miss timing emerges from the handshakes: lookup, dirty-victim
// writeback to the level below (if needed), refill from below, then the
// response. hitLatency of 1 answers hits combinationally, in the same
// cycle as access().

#pragma once

#include "memory/request.h"
#include "sim/config.h" // CacheConfig

#include <cstdint>
#include <vector>

struct CacheStats {
  uint64_t accesses = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t dirtyEvictions = 0;
  uint64_t bytesRead = 0;    // refill traffic from the level below
  uint64_t bytesWritten = 0; // writeback traffic to the level below
  uint64_t latencySum = 0;   // total access-to-response cycles
};

class Cache : public MemPort {
public:
  Cache(const char *name, const CacheConfig &cfg, MemPort *below);

  bool canAccept() const override { return state == IDLE; }
  void access(const MemRequest &req) override;
  bool done() const override { return respReady; }
  MemResponse response() override;
  void tick() override;

  // Functional read for the simulator's own use (syscall 3 string
  // reads): returns true and the byte if this cache holds the line
  bool peek8(uint32_t addr, uint8_t &out) const;

  const char *name;
  CacheStats stats;

private:
  enum State {
    IDLE,        // no access in flight
    HIT_WAIT,    // hit, counting out hitLatency
    WB_ISSUE,    // miss, dirty victim: waiting to send the writeback
    WB_WAIT,     // writeback sent, waiting for the ack
    REFILL_ISSUE,// waiting to send the refill read
    REFILL_WAIT, // refill sent, waiting for the line
    INSTALL,     // full-line write miss: install without refill
    RESPOND,     // response ready, waiting to be consumed
  };

  uint32_t setOf(uint32_t addr) const { return (addr / cfg.lineBytes) % sets; }
  uint32_t tagOf(uint32_t addr) const { return (addr / cfg.lineBytes) / sets; }
  uint32_t lineAddrOf(uint32_t set, uint32_t way) const {
    return (tags[set * cfg.ways + way] * sets + set) * cfg.lineBytes;
  }
  uint8_t *lineData(uint32_t set, uint32_t way) {
    return data.data() + (size_t)(set * cfg.ways + way) * cfg.lineBytes;
  }
  const uint8_t *lineData(uint32_t set, uint32_t way) const {
    return data.data() + (size_t)(set * cfg.ways + way) * cfg.lineBytes;
  }

  int findWay(uint32_t set, uint32_t tag) const;
  uint32_t victimWay(uint32_t set) const;
  void touchLRU(uint32_t set, uint32_t way) { lru[set * cfg.ways + way] = ++lruTick; }

  void finishOnLine();  // perform cur on (curSet, curWay), ready the response
  void sendWriteback();
  void sendRefill();
  void installRefill(MemResponse r);
  void installFullLineWrite();

  CacheConfig cfg;
  uint32_t sets;
  MemPort *below;

  std::vector<uint8_t> data;
  std::vector<uint32_t> tags;
  std::vector<uint8_t> valid, dirty;
  std::vector<uint64_t> lru;
  uint64_t lruTick = 0;

  State state = IDLE;
  MemRequest cur;
  uint32_t curSet = 0, curWay = 0;
  bool curFullLineWrite = false;
  uint32_t waitCtr = 0;
  bool respReady = false;
  MemResponse resp;
  uint64_t tickCount = 0, accessTick = 0;
};
