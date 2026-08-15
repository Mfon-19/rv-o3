#include "memory/system.h"

#include <cinttypes>
#include <cstdio>

MemorySystem::MemorySystem(const SimConfig &cfg)
    : backing(cfg.memBytes), flat(cfg.flatMemory),
      dram(backing, flat ? cfg.flatLatency : cfg.dramLatency) {
  if (flat) {
    flatI.emplace(backing, cfg.flatLatency);
    imem = &*flatI;
    dmem = &dram;
  } else {
    l2.emplace("L2", cfg.l2, &dram, SRC_L2);
    l1i.emplace("L1I", cfg.l1i, &*l2, SRC_L1I);
    l1d.emplace("L1D", cfg.l1d, &*l2, SRC_L1D);
    imem = &*l1i;
    dmem = &*l1d;
  }
}

// Bottom-up: advance each level, then route its completions to the
// requester above before that requester's own tick
void MemorySystem::tick() {
  dram.tick();
  if (flat) {
    flatI->tick();
    return;
  }
  while (dram.hasResponse())
    l2->deliverBelowResponse(dram.response());
  l2->tick();
  while (l2->hasResponse()) {
    MemResponse r = l2->response();
    (r.src == SRC_L1I ? l1i : l1d)->deliverBelowResponse(r);
  }
  l1i->tick();
  l1d->tick();
}

uint8_t MemorySystem::peek8(uint32_t addr) const {
  backing.check(addr, 1, "load");
  uint8_t b;
  if (l1d && l1d->peek8(addr, b))
    return b;
  if (l2 && l2->peek8(addr, b))
    return b;
  return backing.load8(addr);
}

void MemorySystem::printStats(uint64_t retired) const {
  if (flat)
    return;
  const Cache *levels[] = {&*l1i, &*l1d, &*l2};
  for (const Cache *c : levels) {
    const CacheStats &s = c->stats;
    const double missRate = s.accesses ? 100.0 * s.misses / s.accesses : 0.0;
    const double mpki = retired ? 1000.0 * s.misses / retired : 0.0;
    const double avgLat = s.accesses ? (double)s.latencySum / s.accesses : 0.0;
    fprintf(stderr,
            "--- rvsim: %-3s %" PRIu64 " accesses, %" PRIu64
            " misses (%.2f%%, %.2f MPKI), %" PRIu64 " merged, %" PRIu64
            " hit-under-miss, %" PRIu64 " overlap cycles, %" PRIu64
            " dirty evictions, %" PRIu64 " B in, %" PRIu64
            " B out, avg latency %.2f cycles\n",
            c->name, s.accesses, s.misses, missRate, mpki, s.mergedMisses,
            s.hitUnderMiss, s.overlapCycles, s.dirtyEvictions, s.bytesRead,
            s.bytesWritten, avgLat);
  }
}
