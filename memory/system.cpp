// Memory hierarchy construction, cycle-by-cycle response routing, coherent
// side-channel peeking, and cache statistics reporting.

#include "memory/system.h"

#include <cinttypes>
#include <cstdio>

MemorySystem::MemorySystem(const SimConfig &config)
    : backing(config.memBytes),
      flat(config.flatMemory),
      dram(backing, flat ? config.flatLatency : config.dramLatency,
           flat ? config.dataPorts : 1) {
  if (flat) {
    flatInstrMem.emplace(backing, config.flatLatency);
    imem = &*flatInstrMem;
    dmem = &dram;
  } else {
    l2.emplace("L2", config.l2, &dram, SRC_L2);
    l1i.emplace("L1I", config.l1i, &*l2, SRC_L1I);
    l1d.emplace("L1D", config.l1d, &*l2, SRC_L1D);
    imem = &*l1i;
    dmem = &*l1d;
  }
}

void MemorySystem::tick() {
  dram.tick();

  if (flat) {
    flatInstrMem->tick();
    return;
  }

  auto drainDram = [&] {
    while (dram.hasResponse()) {
      l2->deliverBelowResponse(dram.frontResponse());
      dram.popResponse();
    }
  };

  auto drainL2 = [&] {
    while (l2->hasResponse()) {
      const MemResponse &response = l2->frontResponse();
      (response.src == SRC_L1I ? l1i : l1d)->deliverBelowResponse(response);
      l2->popResponse();
    }
  };

  // Drain responses both before and after each level ticks so 1-cycle hits
  // propagate cleanly up the hierarchy without extra bubble cycles.
  drainDram();
  l2->tick();
  drainDram();

  drainL2();
  l1i->tick();
  l1d->tick();
  drainL2();
}

uint8_t MemorySystem::peek8(uint32_t addr) const {
  backing.check(addr, 1, "load");

  uint8_t byte = 0;
  if (l1d && l1d->peek8(addr, byte))
    return byte;
  if (l2 && l2->peek8(addr, byte))
    return byte;

  return backing.load8(addr);
}

void MemorySystem::printStats(uint64_t retired) const {
  if (flat)
    return;

  for (const Cache *cache : {&*l1i, &*l1d, &*l2}) {
    const CacheStats &cacheStats = cache->stats;
    const double missRate =
        cacheStats.accesses ? 100.0 * cacheStats.misses / cacheStats.accesses : 0.0;
    const double mpki = retired ? 1000.0 * cacheStats.misses / retired : 0.0;
    const double avgLatency =
        cacheStats.accesses ? (double)cacheStats.latencySum / cacheStats.accesses : 0.0;

    fprintf(stderr,
            "--- rvsim: %-3s %" PRIu64 " accesses, %" PRIu64
            " misses (%.2f%%, %.2f MPKI), %" PRIu64 " merged, %" PRIu64
            " hit-under-miss, %" PRIu64 " dirty evictions, avg latency %.2f cycles\n",
            cache->name, cacheStats.accesses, cacheStats.misses, missRate, mpki,
            cacheStats.mergedMisses, cacheStats.hitUnderMiss,
            cacheStats.dirtyEvictions, avgLatency);
  }
}
