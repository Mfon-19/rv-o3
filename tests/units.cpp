// Unit tests for the building blocks the directed tests only observe
// indirectly: rings, slot sets, the issue queue's and LSQ's bitmap
// bookkeeping (checked against brute-force answers over random operation
// sequences), the store buffer, functional units, and cache timing.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <random>
#include <utility>

#include "core/fu.h"
#include "core/iq.h"
#include "core/lsq.h"
#include "core/ring.h"
#include "memory/cache.h"
#include "memory/dram.h"

// Random allocate/pop sequences against a std::deque model, at sizes
// that do and don't divide evenly into the wrap
static void rings() {
  std::mt19937 rng(718);
  for (uint32_t size : {1u, 3u, 16u, 65u, 130u}) {
    Ring<uint32_t> ring(size);
    std::deque<std::pair<uint32_t, uint32_t>> model; // {slot, value}
    for (uint32_t step = 0; step < 3000; step++) {
      const auto action = rng() % 3;
      if (action == 0 && !ring.full()) {
        const auto slot = ring.alloc();
        ring.at(slot) = step;
        model.emplace_back(slot, step);
      } else if (action == 1 && !ring.empty()) {
        ring.popHead();
        model.pop_front();
      } else if (!ring.empty()) {
        ring.popTail();
        model.pop_back();
      }
      assert(ring.count() == model.size());
      assert(ring.full() == (model.size() == size));
      for (size_t k = 0; k < model.size(); k++) {
        assert(ring.nth((uint32_t)k) == model[k].second);
        assert(ring.indexOf((uint32_t)k) == model[k].first);
      }
      for (uint32_t slot = 0; slot < size; slot++) {
        const bool live = std::any_of(model.begin(), model.end(),
                                      [=](auto p) { return p.first == slot; });
        assert(ring.live(slot) == live);
      }
      assert(!ring.live(size));
    }
  }
}

// Collect a visit's slots, optionally removing each as it is visited
template <class Visit> static std::vector<uint32_t> collect(Visit visit) {
  std::vector<uint32_t> out;
  visit([&](uint32_t slot) {
    out.push_back(slot);
    return true;
  });
  return out;
}

// Visits come out in age order from any head, including removal mid-walk
static void slotSets() {
  std::mt19937 rng(9);
  for (uint32_t size : {1u, 3u, 64u, 65u, 130u}) {
    SlotSet set(size);
    std::vector<bool> model(size);
    for (int step = 0; step < 4000; step++) {
      const uint32_t slot = rng() % size;
      if (rng() % 2) {
        set.set(slot);
        model[slot] = true;
      } else {
        set.reset(slot);
        model[slot] = false;
      }
      const uint32_t head = rng() % size, end = rng() % size;
      auto age = [&](uint32_t s) { return (s + size - head) % size; };
      std::vector<uint32_t> byAge, older;
      for (uint32_t k = 0; k < size; k++) {
        const uint32_t s = (head + k) % size;
        if (model[s])
          byAge.push_back(s);
        if (model[s] && age(s) < age(end))
          older.insert(older.begin(), s); // newest first
      }
      assert(collect([&](auto f) { set.visit(head, f); }) == byAge);
      assert(collect([&](auto f) { set.visitOlder(head, end, f); }) == older);
    }
    std::vector<uint32_t> removed;
    for (uint32_t s = 0; s < size; s++)
      set.set(s);
    set.visit(size / 2, [&](uint32_t s) {
      set.reset(s);
      removed.push_back(s);
      return true;
    });
    assert(removed.size() == size && collect([&](auto f) { set.visit(0, f); }).empty());
  }
}

// The bitmap issue queue against a per-entry model: readiness after any
// mix of dispatch, wakeup, issue, flush, and retirement
static void issueQueue() {
  std::mt19937 rng(21);
  const uint32_t robSize = 70, iqSize = 24, pregs = 12;
  IssueQueue iq(iqSize, robSize, pregs);
  Ring<uint64_t> rob(robSize); // slot -> seq, in program order
  struct Model {
    bool valid = false, ready1 = false, ready2 = false;
    uint8_t ps1 = 0, ps2 = 0;
  };
  std::vector<Model> model(robSize);
  uint64_t seq = 0;
  for (int step = 0; step < 20000; step++) {
    const auto action = rng() % 8;
    if (action < 3 && !rob.full() && !iq.full()) { // dispatch
      const uint32_t slot = rob.alloc();
      rob.at(slot) = seq;
      IqEntry *q = iq.allocate(slot);
      q->seq = seq++;
      q->ps1 = (uint8_t)(rng() % pregs);
      q->ps2 = (uint8_t)(rng() % pregs);
      const bool r1 = rng() % 2, r2 = rng() % 2;
      iq.track(q, r1, r2);
      model[slot] = {true, r1, r2, q->ps1, q->ps2};
    } else if (action < 5) { // writeback of a physical register
      const uint8_t p = (uint8_t)(rng() % pregs);
      iq.wakeup(p);
      for (Model &m : model) {
        m.ready1 |= m.valid && m.ps1 == p;
        m.ready2 |= m.valid && m.ps2 == p;
      }
    } else if (action == 5 && !rob.empty()) { // squash a random suffix
      const uint64_t keep = rob.head() + rng() % (seq - rob.head());
      iq.flushYounger(keep);
      while (!rob.empty() && rob.tail() > keep) {
        model[rob.indexOf(rob.count() - 1)].valid = false;
        rob.popTail();
      }
    } else if (!rob.empty() && !model[rob.indexOf(0)].valid) {
      rob.popHead(); // retire an entry that has already issued
    }
    // Issue a random subset of the ready entries, checking the visit order
    std::vector<uint32_t> expect;
    for (uint32_t k = 0; k < rob.count(); k++) {
      const Model &m = model[rob.indexOf(k)];
      if (m.valid && m.ready1 && m.ready2)
        expect.push_back(rob.indexOf(k));
    }
    std::vector<uint32_t> got;
    iq.visitReady(rob.empty() ? 0 : rob.indexOf(0), [&](IqEntry &q) {
      got.push_back(q.robIdx);
      if (rng() % 3 == 0) {
        iq.release(&q);
        model[q.robIdx].valid = false;
      }
      return true;
    });
    assert(got == expect);
    uint32_t live = 0;
    for (uint32_t slot = 0; slot < robSize; slot++) {
      const Model &m = model[slot];
      live += m.valid;
      if (m.valid)
        assert(iq.sourceReady(slot, false) == m.ready1 &&
               iq.sourceReady(slot, true) == m.ready2);
    }
    assert(iq.count() == live);
  }
}

// The LSQ's work sets, counters, and store filter against brute-force
// scans, through the same state changes the core makes
static void lsqWorkSets() {
  std::mt19937 rng(35);
  for (uint32_t size : {3u, 16u, 64u, 65u}) {
    LSQ lsq(size);
    uint64_t seq = 0;
    for (int step = 0; step < 20000; step++) {
      const auto action = rng() % 10;
      auto randomEntry = [&]() -> LsqEntry & { return lsq.nth(rng() % lsq.count()); };
      if (action < 3 && !lsq.full()) { // dispatch
        LsqEntry &le = lsq.at(lsq.alloc());
        le.seq = seq++;
        le.isStore = rng() % 2;
        le.size = (uint8_t)(1u << (rng() % 3));
        if (le.isStore)
          lsq.setStoreSource(le, 0, rng() % 2, 7);
      } else if (lsq.empty()) {
        continue;
      } else if (action == 3) { // an address resolves
        LsqEntry &le = randomEntry();
        if (!le.addrValid) {
          le.addr = (rng() % 64) & ~(le.size - 1u);
          lsq.resolveAddress(le);
        }
      } else if (action == 4) { // a candidate load issues or forwards
        LsqEntry &le = randomEntry();
        if (!le.isStore && le.addrValid && !le.issued && !le.done) {
          if (rng() % 2)
            lsq.issueLoad(le);
          else
            lsq.completeLoad(le, 1);
        }
      } else if (action == 5) { // a cache reply
        LsqEntry &le = randomEntry();
        if (!le.isStore && le.issued && !le.done)
          lsq.completeLoad(le, 2);
      } else if (action == 6) { // writeback takes a finished load
        LsqEntry &le = randomEntry();
        if (!le.isStore && le.done && !le.reported)
          lsq.reportLoad(le);
      } else if (action == 7) { // store data arrives
        LsqEntry &le = randomEntry();
        if (le.isStore && !le.dataReady)
          lsq.captureStoreData(le, 9);
      } else if (action == 8) {
        lsq.popHead();
      } else {
        lsq.popTail();
      }

      std::vector<uint32_t> candidates, updates, accessed;
      uint64_t unknownStore = UINT64_MAX, pendingLoad = UINT64_MAX;
      for (uint32_t k = 0; k < lsq.count(); k++) {
        const LsqEntry &le = lsq.nth(k);
        const uint32_t slot = lsq.indexOf(k);
        if (le.isStore && !le.addrValid)
          unknownStore = std::min(unknownStore, le.seq);
        if (!le.isStore && le.issued && !le.done)
          pendingLoad = std::min(pendingLoad, le.seq);
        if (!le.isStore && le.addrValid && !le.issued && !le.done)
          candidates.push_back(slot);
        if (le.isStore ? !le.dataReady : le.done && !le.reported)
          updates.push_back(slot);
        if (!le.isStore && (le.issued || le.done))
          accessed.push_back(slot);
      }
      assert(collect([&](auto f) { lsq.visitCandidates(f); }) == candidates);
      assert(collect([&](auto f) { lsq.visitAccessedLoads(f); }) == accessed);
      std::vector<uint32_t> gotUpdates;
      lsq.visitUpdates([&](LsqEntry &le) {
        gotUpdates.push_back(uint32_t(&le - &lsq.at(0)));
        return true;
      });
      assert(gotUpdates == updates);
      assert(lsq.oldestUnknownStore() == unknownStore);
      assert(lsq.oldestPendingLoad() == pendingLoad);
      assert(lsq.hasPendingLoads() == (pendingLoad != UINT64_MAX));
      assert(lsq.needsUpdate() == !updates.empty());
      for (uint32_t k = 0; k < lsq.count(); k++) {
        std::vector<uint32_t> older;
        for (uint32_t j = k; j-- > 0;)
          if (lsq.nth(j).isStore && lsq.nth(j).addrValid)
            older.push_back(lsq.indexOf(j));
        std::vector<uint32_t> gotOlder;
        lsq.visitOlderStores(lsq.indexOf(k), [&](const LsqEntry &st) {
          gotOlder.push_back(uint32_t(&st - &lsq.at(0)));
          return true;
        });
        assert(gotOlder == older);
        const LsqEntry &st = lsq.nth(k);
        if (st.isStore && st.addrValid) // the filter never hides a store
          assert(lsq.mayStoreToWord(st.addr) &&
                 lsq.mayStoreToWord(st.addr + st.size - 1));
      }
    }
  }
}

// Stores issue in order, and an out-of-order ack never moves the issue
// cursor or lets a younger entry pop first
static void storeBuffer() {
  StoreBuffer sb(3);
  for (unsigned lap = 0; lap < 20; lap++) {
    for (unsigned i = 0; i < 3; i++) {
      StoreBufEntry entry;
      entry.data = lap * 3 + i;
      sb.push(entry);
    }
    for (unsigned i = 0; i < 3; i++) {
      assert(sb.nextToIssue()->data == lap * 3 + i);
      assert(&sb.at(sb.nextIssueSlot()) == sb.nextToIssue());
      sb.issue();
    }
    assert(sb.nextToIssue() == nullptr);
    sb.nth(1).acked = true;
    assert(!sb.head().acked);
    sb.head().acked = true;
    sb.popHead();
    sb.popHead();
    assert(sb.nextToIssue() == nullptr && sb.count() == 1);
    sb.popHead();
  }
}

static FuOp op(uint64_t seq) {
  FuOp o;
  o.valid = true;
  o.seq = seq;
  return o;
}

// Latency, throughput, output-slot backpressure, and squash
static void functionalUnits() {
  for (uint32_t latency : {1u, 3u, 7u, 65u}) {
    for (bool pipelined : {false, true}) {
      FuUnit unit("test", latency, pipelined);
      unit.tick();
      assert(unit.busyCycles == 0 && !unit.out.valid);
      unit.accept(op(1));
      for (uint32_t i = 1; i < latency; i++) {
        unit.tick();
        assert(!unit.out.valid);
      }
      unit.tick();
      assert(unit.out.valid && unit.out.seq == 1 && !unit.busy());
      if (pipelined) {
        // A held output freezes the pipe behind it
        unit.accept(op(2));
        for (unsigned i = 0; i < 20; i++)
          unit.tick();
        assert(unit.out.seq == 1 && unit.busy());
        assert(unit.busyCycles == latency + 20);
        unit.flushYounger(1); // squash the held op, keep the older output
        assert(!unit.busy());
      } else {
        assert(!unit.canAccept()); // non-pipelined: busy until drained
      }
      unit.out.valid = false;
      unit.accept(op(3));
      unit.flushYounger(2);
      for (uint32_t i = 0; i <= latency; i++)
        unit.tick();
      assert(!unit.out.valid && !unit.busy() && unit.canAccept());
    }
  }
  // A pipelined unit starts one op per cycle and finishes them in order
  FuUnit pipe("test", 3, true);
  for (uint64_t seq = 1; seq <= 20; seq++) {
    if (seq > 3) {
      assert(pipe.out.valid && pipe.out.seq == seq - 3);
      pipe.out.valid = false;
    }
    assert(pipe.canAccept());
    pipe.accept(op(seq));
    pipe.tick();
  }
}

// Advance a cache over its DRAM, routing refills up as MemorySystem does
static void tickPair(Cache &cache, DRAM &dram) {
  dram.tick();
  while (dram.hasResponse()) {
    cache.deliverBelowResponse(dram.frontResponse());
    dram.popResponse();
  }
  cache.tick();
}

static MemRequest lineWrite(uint8_t fill, uint64_t tag = 0) {
  MemRequest w;
  w.isWrite = true;
  w.size = 64;
  w.tag = tag;
  w.wline.fill(fill);
  return w;
}

// Hits answer after exactly hitLatency, in order, and a read's data is
// captured when it is performed, not when it is popped
static void hitLatency(uint32_t latency) {
  Memory memory(4096);
  DRAM dram(memory, 3);
  Cache cache("test", CacheConfig{256, 1, 64, latency, 4, 4}, &dram, 1);
  cache.access(lineWrite(17)); // a full-line write installs without a refill
  for (uint32_t t = 1; t < latency; t++) {
    assert(!cache.hasResponse());
    cache.tick();
  }
  assert(cache.hasResponse());
  cache.popResponse();

  const uint32_t sizes[] = {4, 8, 32, 64};
  for (uint64_t tag = 1; tag <= 100; tag++) {
    MemRequest read;
    read.tag = tag;
    read.size = sizes[tag % 4];
    cache.access(read);
  }
  cache.access(lineWrite(42, 101)); // must not change the reads above
  for (uint32_t t = 1; t < latency; t++) {
    assert(!cache.hasResponse());
    cache.tick();
  }
  for (uint64_t tag = 1; tag <= 101; tag++) {
    const MemResponse &r = cache.frontResponse();
    assert(r.tag == tag);
    if (tag < 101 && sizes[tag % 4] == 4)
      assert(r.rdata == 0x11111111);
    else if (tag < 101)
      for (uint32_t i = 0; i < sizes[tag % 4]; i++)
        assert(r.rline[i] == 17);
    cache.popResponse();
  }
  assert(!cache.hasResponse());
}

// Requests merged into one MSHR are answered in arrival order, each
// seeing the writes that arrived before it
static void mergedMisses() {
  Memory memory(4096);
  memory.store(0, 4, 17);
  DRAM dram(memory, 3);
  Cache cache("test", CacheConfig{256, 1, 64, 4, 4, 4}, &dram, 1);
  for (uint64_t tag = 1; tag <= 4; tag++) {
    MemRequest req;
    req.tag = tag;
    req.size = tag == 1 ? 8 : 4;
    req.isWrite = tag == 3;
    req.wdata = 42;
    cache.access(req);
  }
  for (unsigned t = 1; t <= 3; t++) {
    assert(!cache.hasResponse());
    tickPair(cache, dram);
  }
  for (uint64_t tag = 1; tag <= 4; tag++) {
    const MemResponse &r = cache.frontResponse();
    assert(r.tag == tag);
    if (tag == 1)
      assert(r.rline[0] == 17);
    if (tag == 2)
      assert(r.rdata == 17);
    if (tag == 4)
      assert(r.rdata == 42);
    cache.popResponse();
  }
  assert(!cache.hasResponse() && cache.stats.mergedMisses == 3);
}

// A hit overtakes an older miss
static void hitUnderMiss() {
  Memory memory(4096);
  DRAM dram(memory, 3);
  Cache cache("test", CacheConfig{256, 1, 64, 1, 4, 4}, &dram, 1);
  cache.access(lineWrite(17));
  cache.popResponse();
  MemRequest miss;
  miss.addr = 128;
  miss.size = 4;
  miss.tag = 1;
  cache.access(miss);
  MemRequest hit;
  hit.size = 4;
  hit.tag = 2;
  cache.access(hit);
  assert(cache.frontResponse().tag == 2);
  cache.popResponse();
  assert(!cache.hasResponse());
  for (unsigned t = 0; t < 3; t++)
    tickPair(cache, dram);
  assert(cache.frontResponse().tag == 1 && cache.stats.hitUnderMiss == 1);
  cache.popResponse();
  assert(!cache.hasResponse());
}

int main() {
  rings();
  slotSets();
  issueQueue();
  lsqWorkSets();
  storeBuffer();
  functionalUnits();
  hitLatency(1);
  hitLatency(4);
  mergedMisses();
  hitUnderMiss();
  puts("units: rings, slot sets, issue queue, LSQ work sets, store buffer, "
       "functional units, cache timing passed");
}
