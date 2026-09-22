// Host-side representations must preserve queue and pipeline behavior.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <random>
#include <utility>
#include <vector>

#include "core/fu.h"
#include "core/iq.h"
#include "core/lsq.h"
#include "core/ring.h"
#include "memory/request.h"

static void payloads() {
  std::vector<uint8_t> source(1024);
  for (size_t i = 0; i < source.size(); i++)
    source[i] = (uint8_t)(i * 17);
  ReadPayload reusable;
  for (size_t size : {0u, 8u, 32u, 33u, 64u, 256u, 1024u, 8u, 0u}) {
    reusable.assign(source.data(), source.data() + size);
    ReadPayload copied = reusable;
    ReadPayload assigned;
    assigned = reusable;
    ReadPayload moved = std::move(copied);
    assert(copied.size() == 0);
    copied = std::move(assigned);
    assert(assigned.size() == 0);
    reusable.assign(source.data(), source.data());
    for (const auto *p : {&moved, &copied}) {
      assert(p->size() == size);
      for (size_t i = 0; i < size; i++)
        assert((*p)[i] == source[i]);
    }
    ReadPayload *alias = &moved;
    moved = *alias;
    moved = std::move(*alias);
    assert(moved.size() == size);
  }
  // Queue moves/reallocations must retain independent response ownership.
  std::vector<ReadPayload> queue;
  for (size_t size : {8u, 64u, 32u, 256u, 16u}) {
    ReadPayload p;
    p.assign(source.data(), source.data() + size);
    queue.push_back(std::move(p));
  }
  std::fill(source.begin(), source.end(), 0);
  for (const auto &p : queue)
    for (size_t i = 0; i < p.size(); i++)
      assert(p[i] == (uint8_t)(i * 17));
}

static void rings() {
  std::mt19937 rng(718);
  for (uint32_t size : {1u, 3u, 16u, 65u, 130u}) {
    Ring<uint32_t> ring(size);
    std::deque<std::pair<uint32_t, uint32_t>> model;
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
    // Exercise a full ring wrapping repeatedly (the random walk stays small).
    while (!ring.empty()) ring.popHead();
    for (uint32_t i = 0; i < size; i++) ring.push(i);
    for (uint32_t i = 0; i < size * 4; i++) {
      assert(ring.head() == i);
      ring.popHead();
      ring.push(i + size);
    }
  }
}

static void issueQueues() {
  std::mt19937 rng(902);
  for (uint32_t size : {1u, 16u, 63u, 64u, 65u, 130u}) {
    const uint32_t robSize = size + 7;
    IssueQueue iq(size, robSize, 64);
    Ring<uint64_t> rob(robSize);
    std::vector<IqEntry> model(robSize);
    uint64_t seq = 0;
    auto allocate = [&] {
      const uint32_t slot = rob.alloc();
      assert(!model[slot].valid);
      IqEntry *q = iq.allocate(slot);
      assert(q && q->robIdx == slot);
      q->seq = seq++;
      rob.at(slot) = q->seq;
      q->ps1 = rng() % 64;
      q->ps2 = rng() % 64;
      q->ready1 = rng() % 2;
      q->ready2 = rng() % 2;
      model[slot] = *q;
      iq.track(q);
    };
    // Force live entries on both sides of 64-bit boundaries.
    for (uint32_t i = 0; i < size; i++) allocate();
    assert(iq.full() && iq.allocate(rob.indexOf(rob.count())) == nullptr);
    for (unsigned step = 0; step < 10000; step++) {
      switch (rng() % 5) {
      case 0:
        if (!iq.full() && !rob.full()) allocate();
        break;
      case 1: {
        const uint8_t preg = rng() % 64;
        iq.wakeup(preg);
        for (auto &q : model) if (q.valid) {
          if (q.ps1 == preg) q.ready1 = true;
          if (q.ps2 == preg) q.ready2 = true;
        }
        break;
      }
      case 2: {
        const uint64_t cutoff = seq > 8 ? seq - rng() % 9 : 0;
        iq.flushYounger(cutoff);
        for (auto &q : model)
          if (q.valid && q.seq > cutoff) q.valid = false;
        while (!rob.empty() && rob.tail() > cutoff) rob.popTail();
        break;
      }
      case 3: {
        const size_t slot = rng() % robSize;
        if (model[slot].valid) {
          iq.release(&iq.e[slot]);
          model[slot].valid = false;
        }
        break;
      }
      default: {
        std::vector<uint64_t> expected;
        for (const auto &q : model)
          if (q.valid && q.ready1 && q.ready2) expected.push_back(q.seq);
        std::sort(expected.begin(), expected.end());
        const size_t limit = 1 + rng() % 4;
        size_t issued = 0;
        iq.visitReady(rob.indexOf(0), [&](IqEntry &q) {
          assert(issued < expected.size() && q.seq == expected[issued]);
          model[q.robIdx].valid = false;
          iq.release(&q);
          return ++issued < limit;
        });
        assert(issued == std::min(limit, expected.size()));
        break;
      }
      }
      // Issued entries leave the IQ but keep their ROB slot until retirement.
      while (!rob.empty() && !model[rob.indexOf(0)].valid) rob.popHead();
      std::vector<IqEntry *> ready;
      iq.visitReady(rob.indexOf(0), [&](IqEntry &entry) {
        ready.push_back(&entry);
        return true;
      });
      std::vector<uint64_t> expected;
      size_t n = 0;
      for (size_t i = 0; i < robSize; i++) {
        const auto &m = model[i], &q = iq.e[i];
        assert(m.valid == q.valid);
        if (!m.valid) continue;
        n++;
        assert(m.seq == q.seq && m.ready1 == q.ready1 && m.ready2 == q.ready2);
        if (m.ready1 && m.ready2) expected.push_back(m.seq);
      }
      assert(iq.count() == n && iq.full() == (n == size));
      std::sort(expected.begin(), expected.end());
      assert(expected.size() == ready.size());
      for (size_t i = 0; i < ready.size(); i++) assert(ready[i]->seq == expected[i]);
    }
  }
}

static void pendingLoads() {
  LSQ queue(3);
  const auto first = queue.alloc();
  queue.issueLoad(queue.at(first));
  const auto second = queue.alloc();
  queue.issueLoad(queue.at(second));
  queue.completeLoad(queue.at(second), 42); // completions can arrive out of order
  assert(queue.hasPendingLoads() && queue.needsUpdate());
  queue.popTail(); // completed load: nothing pending to remove
  assert(queue.hasPendingLoads() && !queue.needsUpdate());
  const auto generation = queue.at(first).gen;
  queue.popTail(); // squash the pending load before its response arrives
  assert(!queue.hasPendingLoads());
  const auto reused = queue.alloc();
  assert(reused == first && queue.at(reused).gen != generation);
  queue.issueLoad(queue.at(reused));
  queue.completeLoad(queue.at(reused), 7);
  assert(!queue.hasPendingLoads() && queue.at(reused).value == 7);
  queue.reportLoad(queue.at(reused));
  assert(!queue.needsUpdate());
  queue.popHead();
  assert(queue.empty());

  auto slot = queue.alloc();
  queue.completeLoad(queue.at(slot), 3); // a forwarded load never issued to cache
  assert(queue.needsUpdate() && !queue.hasPendingLoads());
  queue.popTail();
  assert(!queue.needsUpdate());

  slot = queue.alloc();
  auto &store = queue.at(slot);
  store.isStore = true;
  queue.setStoreSource(store, 4, false, 0);
  assert(queue.needsUpdate());
  queue.captureStoreData(store, 99);
  assert(!queue.needsUpdate() && store.data == 99);
  queue.popHead();
  slot = queue.alloc();
  queue.at(slot).isStore = true;
  queue.setStoreSource(queue.at(slot), 5, false, 0);
  queue.popTail(); // squash a store still waiting for its operand
  assert(!queue.needsUpdate());
}

static FuOp operation(uint64_t seq) {
  FuOp op;
  op.valid = true;
  op.seq = seq;
  op.value = (uint32_t)seq * 17;
  return op;
}

static void functionalUnits() {
  for (uint32_t latency : {1u, 3u, 7u, 65u}) {
    for (bool pipelined : {false, true}) {
      FuUnit unit("test", latency, pipelined);
      for (unsigned i = 0; i < 100; i++) unit.tick();
      assert(unit.busyCycles == 0);
      unit.accept(operation(1));
      for (uint32_t i = 1; i < latency; i++) {
        unit.tick();
        assert(!unit.out.valid);
      }
      unit.tick();
      assert(unit.out.valid && unit.out.seq == 1 && !unit.busy());
      if (pipelined) {
        assert(unit.canAccept());
        unit.accept(operation(2));
        for (unsigned i = 0; i < 20; i++) unit.tick();
        assert(unit.out.seq == 1 && unit.busy());
        assert(unit.busyCycles == latency + 20);
        unit.flushYounger(1); // discard the held pipeline op, retain output
        assert(!unit.busy());
      } else {
        assert(!unit.canAccept());
      }
      const auto busy = unit.busyCycles;
      for (unsigned i = 0; i < 100; i++) unit.tick();
      assert(unit.out.valid && unit.out.seq == 1 && unit.busyCycles == busy);
      unit.out.valid = false;
      unit.accept(operation(3));
      unit.flushYounger(2);
      for (uint32_t i = 0; i <= latency; i++) unit.tick();
      assert(!unit.out.valid && !unit.busy() && unit.canAccept());
      assert(unit.busyCycles == busy);
    }
  }
  FuUnit pipe("test", 3, true);
  for (uint64_t seq = 1; seq <= 20; seq++) {
    if (seq > 3) {
      assert(pipe.out.valid && pipe.out.seq == seq - 3);
      pipe.out.valid = false;
    }
    assert(pipe.canAccept());
    pipe.accept(operation(seq));
    pipe.tick();
  }
}

int main() {
  payloads();
  rings();
  issueQueues();
  pendingLoads();
  functionalUnits();
  puts("host structures: payloads, rings, IQ age/readiness, pending loads, FU timing passed");
}
