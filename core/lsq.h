// The load/store queue and the store buffer; together they carry the
// memory-ordering rules.
//
// Memory ops allocate an LSQ entry at dispatch, in program order. The
// AGU fills in the address when the address operand is ready; a
// store's data arrives separately, whenever its producer writes back.
// The rules every mode shares:
//
//   - An older store to exactly the load's address and size hands the
//     load its data directly (a forward). A partial overlap makes the
//     load wait until that store has drained to the cache; there is no
//     byte merging across stores.
//   - Stores touch memory only after they commit: commit moves them
//     to the store buffer, which drains to the data port in order.
//
// How far a load may run ahead of older stores whose addresses are
// not known yet is the configurable part (SimConfig::memOrder): wait
// for all of them (Conservative, which also issues loads one at a
// time), pass only stores proven not to overlap (Bypass), or guess
// that unknown addresses will not overlap and re-execute the load if
// the guess was wrong (Speculative, the default). The re-execution is
// called a replay and reuses the branch recovery machinery.

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/ring.h"
#include "core/slot_set.h"
#include "isa/isa.h"

struct LsqEntry {
  uint64_t seq = 0;
  uint32_t robIdx = 0;
  uint32_t pc = 0;
  uint32_t addr = 0;
  // Stores: address and data resolve independently. The data operand's
  // physical register is watched until it is ready; it is written
  // exactly once while this store is live, so a late capture is
  // always safe
  uint32_t data = 0;
  uint32_t value = 0;
  uint16_t gen = 0;       // slot generation: rejects squashed loads' responses
  Op op = Op::ILLEGAL;    // load extension needs only the opcode
  uint8_t pdst = 0xFF;    // loads: where the result goes
  uint8_t size = 4;
  uint8_t dataPreg = 0xFF;
  bool isStore = false;
  bool addrValid = false;
  bool dataReady = true;
  bool done = false;     // loads: value produced (forwarded or from cache)
  bool issued = false;   // loads: access in flight at the cache
  bool reported = false; // loads: handed to the writeback arbiter
};

// Do two accesses touch any byte in common?
inline bool overlaps(uint32_t a, uint32_t aSize, uint32_t b, uint32_t bSize) {
  return a + aSize > b && b + bSize > a;
}

// A ring of LsqEntry whose slots carry a generation number, so the
// response to a squashed load's cache access cannot be mistaken for
// the slot's next occupant's.
//
// Work sets. Rather than scanning the whole queue for each question the
// core asks every cycle, each question has a bitmap over slots (a
// SlotSet), kept current as entries change state:
//   candidates     loads with an address, not yet issued or completed
//   pending        loads issued to the cache, awaiting data
//   updates        completed loads not yet reported to writeback, and
//                  stores still waiting for their data operand
//   knownStores    stores whose address has resolved
//   unknownStores  stores whose address has not
//   accessedLoads  loads that issued or completed (replay candidates)
// The counters pendingLoads, unreportedLoads, and waitingStores count
// members of pending and updates, so "is there anything to do?" is free.
// storeWords counts resolved stores per 4-byte word bucket, so a load
// that no resolved store can overlap skips the forwarding search. Every
// state change goes through the methods below, and remove() takes a
// retired or squashed entry out of all of them.
class LSQ : public Ring<LsqEntry> {
public:
  explicit LSQ(uint32_t size)
      : Ring(size), genCtr(size, 0), candidates(size), pending(size),
        updates(size), knownStores(size), unknownStores(size), accessedLoads(size) {}

  uint32_t alloc() {
    const uint32_t idx = Ring::alloc();
    at(idx).gen = ++genCtr[idx]; // stale responses to this slot die here
    return idx;
  }

  void issueLoad(LsqEntry &load) {
    candidates.reset(slotOf(load));
    pending.set(slotOf(load));
    accessedLoads.set(slotOf(load));
    load.issued = true;
    pendingLoads++;
  }

  // A load's value arrived (cache reply, forward, or a faulted load's zero);
  // it reaches writeback on the next update pass
  void completeLoad(LsqEntry &load, uint32_t value) {
    candidates.reset(slotOf(load));
    pending.reset(slotOf(load));
    updates.set(slotOf(load));
    accessedLoads.set(slotOf(load));
    load.value = value;
    load.done = true;
    if (load.issued)
      pendingLoads--;
    unreportedLoads++;
  }

  void reportLoad(LsqEntry &load) {
    updates.reset(slotOf(load));
    load.reported = true;
    unreportedLoads--;
  }

  void setStoreSource(LsqEntry &store, uint8_t preg, bool ready, uint32_t value) {
    unknownStores.set(slotOf(store));
    if (!ready)
      updates.set(slotOf(store));
    store.dataPreg = preg;
    store.dataReady = ready;
    if (ready)
      store.data = value;
    else
      waitingStores++;
  }

  void captureStoreData(LsqEntry &store, uint32_t value) {
    updates.reset(slotOf(store));
    store.data = value;
    store.dataReady = true;
    waitingStores--;
  }

  void resolveAddress(LsqEntry &entry) {
    entry.addrValid = true;
    const uint32_t slot = slotOf(entry);
    if (entry.isStore) {
      unknownStores.reset(slot);
      knownStores.set(slot);
      storeWords[wordBucket(entry.addr)]++;
      if (wordBucket(entry.addr) != wordBucket(entry.addr + entry.size - 1))
        storeWords[wordBucket(entry.addr + entry.size - 1)]++;
    } else {
      candidates.set(slot);
    }
  }

  template <class F> void visitUpdates(F &&f) {
    updates.visit(indexOf(0), [&](uint32_t slot) { return f(at(slot)); });
  }
  template <class F> void visitCandidates(F &&f) {
    candidates.visit(indexOf(0), f);
  }
  template <class F> void visitOlderStores(uint32_t end, F &&f) {
    knownStores.visitOlder(indexOf(0), end,
                          [&](uint32_t slot) { return f(at(slot)); });
  }
  // False proves no resolved store overlaps the aligned word at addr (a
  // non-faulting load never spans two words). True may be a bucket
  // collision; the caller then does the full search. A misaligned
  // speculative store is counted in both words it touches
  bool mayStoreToWord(uint32_t addr) const { return storeWords[wordBucket(addr)] != 0; }
  template <class F> void visitAccessedLoads(F &&f) const {
    accessedLoads.visit(indexOf(0), f);
  }
  uint64_t oldestUnknownStore() { return oldestSeq(unknownStores); }
  uint64_t oldestPendingLoad() { return oldestSeq(pending); }

  void popHead() {
    remove(head());
    Ring::popHead();
  }
  void popTail() {
    remove(tail());
    Ring::popTail();
  }

  bool hasPendingLoads() const { return pendingLoads != 0; }
  bool needsUpdate() const { return unreportedLoads != 0 || waitingStores != 0; }

private:
  uint32_t slotOf(const LsqEntry &entry) { return uint32_t(&entry - &at(0)); }
  uint64_t oldestSeq(const SlotSet &set) {
    uint64_t seq = UINT64_MAX;
    set.visit(indexOf(0), [&](uint32_t slot) { seq = at(slot).seq; return false; });
    return seq;
  }
  void remove(const LsqEntry &entry) {
    const uint32_t slot = slotOf(entry);
    candidates.reset(slot);
    pending.reset(slot);
    updates.reset(slot);
    knownStores.reset(slot);
    unknownStores.reset(slot);
    accessedLoads.reset(slot);
    if (entry.isStore && entry.addrValid) {
      storeWords[wordBucket(entry.addr)]--;
      if (wordBucket(entry.addr) != wordBucket(entry.addr + entry.size - 1))
        storeWords[wordBucket(entry.addr + entry.size - 1)]--;
    }
    if (entry.issued && !entry.done)
      pendingLoads--;
    if (!entry.isStore && entry.done && !entry.reported)
      unreportedLoads--;
    if (entry.isStore && !entry.dataReady)
      waitingStores--;
  }

  std::vector<uint16_t> genCtr;
  SlotSet candidates, pending, updates, knownStores, unknownStores;
  SlotSet accessedLoads;
  static uint32_t wordBucket(uint32_t addr) { return (addr >> 2) & 63; }
  std::array<uint32_t, 64> storeWords{}; // resolved stores per bucket
  uint32_t pendingLoads = 0;
  uint32_t unreportedLoads = 0, waitingStores = 0;
};

// Committed stores waiting to be written to the cache: issued in
// order, several in flight at once. Entries stay (visible to load
// forwarding) until the cache acknowledges, and pop in order. A full
// store buffer stalls commit and gets priority for the data port, so
// it always drains eventually
struct StoreBufEntry {
  uint32_t addr = 0;
  uint8_t size = 4;
  uint32_t data = 0;
  bool inflight = false; // issued to the cache
  bool acked = false;    // cache applied it; pop when it reaches head
  uint32_t txn = 0;      // matches the ack to this entry
};

class StoreBuffer : public Ring<StoreBufEntry> {
public:
  explicit StoreBuffer(uint32_t size) : Ring(size) {}
  StoreBufEntry *nextToIssue() { return issued < count() ? &nth(issued) : nullptr; }
  uint32_t nextIssueSlot() const { return indexOf(issued); }
  void issue() { nth(issued++).inflight = true; }
  void popHead() { --issued; Ring::popHead(); }
private:
  // Issued stores form a prefix even when their acks return out of order.
  uint32_t issued = 0;
};
