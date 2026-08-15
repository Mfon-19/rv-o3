// A nonblocking, write-back, write-allocate set-associative cache.
//
// One class serves as L1I, L1D, and L2 — an L1's `below` port is the
// L2, the L2's is DRAM. Instead of one blocking state machine, the
// cache is organized around three tables:
//
//   - MSHRs (miss-status holding registers): one per outstanding
//     missed LINE. A second access to a line already being fetched
//     MERGES into the existing entry's waiting list instead of
//     issuing a duplicate refill. MSHRs-full is the backpressure.
//   - a writeback queue: dirty victims park here and drain to the
//     level below opportunistically, off the refill critical path.
//     Lines here have left the arrays, so peek8 scans this queue too.
//   - a response queue: completed accesses (hits after hitLatency,
//     and every waiting request when its refill lands) ready for the
//     requester to pop, tagged, in completion order.
//
// Hit-under-miss falls out of the structure: hits never consult the
// MSHRs. Reads capture their data and writes take effect at access()
// (a hit) or at line install (a miss) — arrival order at each line is
// the order requests take effect, and the core never issues two
// overlapping requests concurrently, so replay order inside an MSHR
// cannot matter.
//
// Deliberate simplifications: canAccept() conservatively requires a
// free MSHR and writeback-queue room even for what turns out to be a
// hit; a full-line write that misses installs immediately without a
// refill (every byte is overwritten); accepted accesses per cycle are
// not limited (the port above already rations itself); writebacks to
// the level below are fire-and-forget (the level below applies them
// on arrival).

#pragma once

#include <deque>

#include "memory/request.h"
#include "sim/config.h" // CacheConfig

#include <cstdint>
#include <vector>

struct CacheStats {
  uint64_t accesses = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t mergedMisses = 0;   // misses absorbed by an existing MSHR
  uint64_t wbqRestores = 0;    // lines pulled back from the wb queue
  uint64_t hitUnderMiss = 0;   // hits served while a miss was pending
  uint64_t overlapCycles = 0;  // cycles with 2+ misses outstanding
  uint64_t dirtyEvictions = 0;
  uint64_t bytesRead = 0;    // refill traffic from the level below
  uint64_t bytesWritten = 0; // writeback traffic to the level below
  uint64_t latencySum = 0;   // total access-to-response cycles
};

class Cache : public MemPort {
public:
  Cache(const char *name, const CacheConfig &cfg, MemPort *below,
        uint8_t srcId);

  bool canAccept() const override {
    return freeMshr() >= 0 && wbq.size() < cfg.wbq;
  }
  void access(const MemRequest &req) override;
  bool hasResponse() const override { return !respQ.empty(); }
  MemResponse response() override;
  void tick() override;

  // A completion from the level below (refill line or writeback ack),
  // routed here by the MemorySystem
  void deliverBelowResponse(const MemResponse &r);

  // Functional read for the simulator's own use (syscall 3 string
  // reads): returns true and the byte if this cache holds the line —
  // in the arrays or still parked in the writeback queue
  bool peek8(uint32_t addr, uint8_t &out) const;

  const char *name;
  CacheStats stats;

private:
  struct Waiting {
    MemRequest req;
    uint64_t issueTick;
  };
  struct Mshr {
    bool valid = false;
    uint32_t lineAddr = 0;
    bool refillSent = false;
    std::vector<Waiting> waiting;
  };
  struct WbEntry {
    uint32_t addr = 0;
    std::vector<uint8_t> line;
  };
  struct HitTxn {
    MemResponse resp;
    uint32_t remaining;
  };
  struct PendingInstall { // refill returned while the wb queue was full
    uint32_t mshrIdx;
    std::vector<uint8_t> line;
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
  void touchLRU(uint32_t set, uint32_t way) {
    lru[set * cfg.ways + way] = ++lruTick;
  }
  int freeMshr() const;
  int mshrFor(uint32_t lineAddr) const;

  // Perform one request on a resident line; returns the response
  MemResponse performOnLine(const MemRequest &req, uint32_t set,
                            uint32_t way);
  // Evict (to the wb queue) and claim a way for lineAddr; false if the
  // wb queue has no room for the dirty victim
  bool claimWay(uint32_t lineAddr, uint32_t &set, uint32_t &way);
  bool tryInstall(uint32_t mshrIdx, const std::vector<uint8_t> &line);
  void finishHit(MemResponse &&resp);

  CacheConfig cfg;
  uint32_t sets;
  MemPort *below;
  uint8_t srcId;

  std::vector<uint8_t> data;
  std::vector<uint32_t> tags;
  std::vector<uint8_t> valid, dirty;
  std::vector<uint64_t> lru;
  uint64_t lruTick = 0;

  std::vector<Mshr> mshrs;
  std::deque<WbEntry> wbq;
  std::deque<PendingInstall> pendingInstalls;
  std::vector<HitTxn> hitPipe;
  std::deque<MemResponse> respQ;
  uint64_t tickCount = 0;
};
