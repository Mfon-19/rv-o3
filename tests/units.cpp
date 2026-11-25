// Focused unit tests for core microarchitectural queues, pipelined functional
// units, and nonblocking cache behaviors (merged misses and hit-under-miss).

#include <cassert>
#include <cstdio>

#include "core/fu.h"
#include "core/iq.h"
#include "core/lsq.h"
#include "core/ring.h"
#include "memory/cache.h"
#include "memory/memory.h"

static void testRingAndStoreBuffer() {
  Ring<uint32_t> ring(4);

  for (uint32_t i = 0; i < 4; i++)
    ring.push(i + 10);

  assert(ring.full() && ring.head() == 10 && ring.tail() == 13);
  ring.popHead();
  ring.push(14);
  assert(ring.head() == 11 && ring.tail() == 14 && ring.nth(2) == 13);
  ring.popTail();
  assert(ring.count() == 3 && ring.tail() == 13);

  StoreBuffer storeBuffer(3);
  for (uint32_t i = 0; i < 3; i++)
    storeBuffer.push(StoreBufferEntry{i * 4, 4, i + 1});

  for (uint32_t i = 0; i < 3; i++) {
    assert(storeBuffer.nextToIssue()->data == i + 1);
    storeBuffer.issue();
  }

  assert(storeBuffer.nextToIssue() == nullptr);
  storeBuffer.popHead();
  assert(storeBuffer.count() == 2);
}

static void testIssueQueue() {
  IssueQueue iq(4);

  iq.push(IqEntry{1, 0, 0, 5, 6, false, true, FuKind::ALU});
  iq.push(IqEntry{2, 1, 0, 6, 7, true, false, FuKind::ALU});
  iq.push(IqEntry{3, 2, 0, 5, 7, false, false, FuKind::ALU});

  iq.wakeup(5);
  assert(iq.entries[0].ready1 && iq.entries[0].ready2);
  assert(iq.entries[2].ready1 && !iq.entries[2].ready2);

  iq.flushYounger(1);
  assert(iq.count() == 1 && iq.entries[0].seq == 1);
}

static void testFunctionalUnits() {
  FuUnit pipe("mul", 3, true);

  for (uint64_t seq = 1; seq <= 8; seq++) {
    if (seq > 3) {
      assert(pipe.output.valid && pipe.output.seq == seq - 3);
      pipe.output.valid = false;
    }

    FuOp op;
    op.valid = true;
    op.seq = seq;
    pipe.accept(op);
    pipe.tick();
  }

  assert(pipe.output.valid && pipe.output.seq == 6);
  pipe.flushYounger(5);
  assert(!pipe.output.valid && !pipe.busy());
}

static void tickPair(Cache &cache, DRAM &dram) {
  dram.tick();
  while (dram.hasResponse()) {
    cache.deliverBelowResponse(dram.frontResponse());
    dram.popResponse();
  }
  cache.tick();
}

static void testCache() {
  Memory memory(4096);
  memory.store(0, 4, 17);
  DRAM dram(memory, 3);
  Cache cache("test", CacheConfig{256, 1, 64, 1, 4, 4}, &dram, 1);

  // Read -> write -> read on the same cold line merge into one MSHR and replay in order.
  for (uint64_t tag = 1; tag <= 3; tag++) {
    MemRequest req;
    req.tag = tag;
    req.size = 4;
    req.isWrite = tag == 2;
    req.writeData = 42;
    cache.access(req);
  }

  for (unsigned cycle = 0; cycle < 3; cycle++)
    tickPair(cache, dram);

  assert(cache.frontResponse().readData == 17);
  cache.popResponse();
  cache.popResponse();
  assert(cache.frontResponse().readData == 42);
  cache.popResponse();
  assert(!cache.hasResponse() && cache.stats.mergedMisses == 2);

  // Verify a hit on line 0 completes immediately while a miss to line 128 is in flight.
  MemRequest missReq;
  missReq.addr = 128;
  missReq.size = 4;
  missReq.tag = 10;
  cache.access(missReq);

  MemRequest hitReq;
  hitReq.addr = 0;
  hitReq.size = 4;
  hitReq.tag = 11;
  cache.access(hitReq);

  assert(cache.frontResponse().tag == 11 && cache.stats.hitUnderMiss == 1);
  cache.popResponse();
}

int main() {
  testRingAndStoreBuffer();
  testIssueQueue();
  testFunctionalUnits();
  testCache();
  puts("units: passed");
}
