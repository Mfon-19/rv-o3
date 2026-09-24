#include "core/ooo.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#include "isa/execute.h"
#include "sim/syscall.h"

OoOCore::OoOCore(const SimConfig &cfg, MemorySystem &msys)
    : cfg(cfg), msys(msys), imem(msys.imem), dmem(msys.dmem),
      showMemStats(!cfg.flatMemory || cfg.flatLatency > 1),
      freeList(cfg.physRegs), prf(cfg.physRegs), rob(cfg.robSize),
      iq(cfg.iqSize, cfg.robSize, cfg.physRegs), lsq(cfg.lsqSize), sb(cfg.sbSize),
      pred(cfg),
      alus(cfg.aluCount, FuUnit("alu", 1, true)), brUnit("br", 1, true),
      mulUnit("mul", cfg.mulLatency, cfg.mulPipelined),
      divUnit("div", cfg.divLatency, false),
      agus(cfg.aguCount, FuUnit("agu", 1, true)),
      fBytes(cfg.fetchBlockBytes()), fetchQ(cfg.fetchQSize),
      loadOuts(cfg.width > 2 ? cfg.width : 2), depTable(cfg.depTableSize, 0),
      trace(cfg.trace) {
  for (FuUnit &u : alus)
    units.push_back(&u);
  for (FuUnit *u : {&brUnit, &mulUnit, &divUnit})
    units.push_back(u);
  wbReady.reserve(units.size() + loadOuts.size());
  rmap.reset(); // architectural register i starts in physical register i...
  for (uint32_t p = 32; p < cfg.physRegs; p++)
    freeList.push((uint8_t)p); // ...and the rest are free
}

int OoOCore::run() {
  while (!halted) {
    if (stats.cycles >= cfg.maxCycles) {
      fprintf(stderr,
              "stopping after %" PRIu64
              " cycles without an exit syscall (raise with -c)\n",
              stats.cycles);
      exitCode = 2;
      break;
    }
    stepCycle();
  }

  const Stats &s = stats;
  auto ratio = [](uint64_t a, uint64_t b, double scale = 1.0) {
    return b ? scale * (double)a / (double)b : 0.0;
  };
  fprintf(stderr, "--- rvsim: %" PRIu64 " cycles, %" PRIu64
          " instructions retired, IPC = %.3f (CPI = %.3f)\n",
          s.cycles, s.retired, ratio(s.retired, s.cycles),
          ratio(s.cycles, s.retired));
  fprintf(stderr, "--- rvsim: dispatch stalls: rob-full %" PRIu64
          ", iq-full %" PRIu64 ", lsq-full %" PRIu64 ", no-preg %" PRIu64
          ", serialize %" PRIu64 ", fetch-empty %" PRIu64 "\n",
          s.dsRobFull, s.dsIqFull, s.dsLsqFull, s.dsNoPreg, s.dsSerialize,
          s.dsFetchEmpty);
  fprintf(stderr, "--- rvsim: %" PRIu64 " branches, %" PRIu64
          " mispredicted (%.1f%%, %.2f MPKI), %" PRIu64
          " flushes; avg issue %.2f; wb-port holds %" PRIu64 "\n",
          s.branches, s.mispredicts, ratio(s.mispredicts, s.branches, 100.0),
          ratio(s.mispredicts, s.retired, 1000.0), s.flushes,
          ratio(s.issuedOps, s.cycles), s.wbConflicts);
  fprintf(stderr, "--- rvsim: %" PRIu64 " dispatched, %" PRIu64
          " wrong-path (%.1f%%); recovery loss %" PRIu64
          " cycles; retire blocked on loads %" PRIu64 " cycles\n",
          seqCtr, seqCtr - s.retired, ratio(seqCtr - s.retired, seqCtr, 100.0),
          s.recoveryLossCycles, s.memRetireStallCycles);
  fprintf(stderr, "--- rvsim: occupancy: rob %.1f/%u, iq %.1f/%u, "
          "lsq %.1f/%u, sb %.1f/%u, pregs %.1f/%u\n",
          ratio(s.robOccSum, s.cycles), cfg.robSize,
          ratio(s.iqOccSum, s.cycles), cfg.iqSize,
          ratio(s.lsqOccSum, s.cycles), cfg.lsqSize,
          ratio(s.sbOccSum, s.cycles), cfg.sbSize,
          ratio(s.pregsUsedSum, s.cycles), cfg.physRegs - 32);
  fprintf(stderr, "--- rvsim: fu busy:");
  auto fuLine = [&](const char *name, uint64_t busy, uint64_t ops) {
    fprintf(stderr, " %s %.1f%% (%" PRIu64 " ops)", name,
            ratio(busy, s.cycles, 100.0), ops);
  };
  uint64_t aluBusy = 0, aluOps = 0;
  for (const FuUnit &u : alus) {
    aluBusy += u.busyCycles;
    aluOps += u.ops;
  }
  fuLine("alu", aluBusy, aluOps);
  for (const FuUnit *u : {&brUnit, &mulUnit, &divUnit})
    fuLine(u->name, u->busyCycles, u->ops);
  uint64_t aguBusy = 0, aguOps = 0;
  for (const FuUnit &u : agus) {
    aguBusy += u.busyCycles;
    aguOps += u.ops;
  }
  fuLine("agu", aguBusy, aguOps);
  fputc('\n', stderr);
  fprintf(stderr, "--- rvsim: %" PRIu64 " loads forwarded from stores, %"
          PRIu64 " speculative loads, %" PRIu64 " replays, %" PRIu64
          " store-buffer commit stalls\n",
          s.loadsForwarded, s.specLoads, s.loadReplays, s.sbCommitStalls);
  if (showMemStats) {
    fprintf(stderr, "--- rvsim: %" PRIu64 " ifetch stall cycles, %" PRIu64
            " data stall cycles\n",
            s.dsFetchEmpty, s.dataStallCycles);
    msys.printStats(s.retired);
  }
  if (s.badFetchWaitCycles)
    fprintf(stderr, "--- rvsim: speculative target wait %" PRIu64 " cycles\n",
            s.badFetchWaitCycles);
  fprintf(stderr, "--- rvsim: exit code %d\n", exitCode);
  return exitCode;
}

void OoOCore::dumpRegs() const {
  uint32_t regs[32];
  for (int i = 0; i < 32; i++)
    regs[i] = reg(i);
  dumpRegisters(regs, pc);
}

// Commit: up to width instructions per cycle from the ROB head, in
// program order. This is where state becomes architectural: mappings
// are freed, stores move to the store buffer (the cache write lands
// later, but the buffer forwards to younger loads meanwhile), syscalls
// run, the predictor trains
void OoOCore::commitStage() {
  uint32_t done = 0;
  while (done < cfg.width && !rob.empty() && rob.head().done && !halted) {
    RobEntry &e = rob.head();
    const Instr &I = e.ins;

    // A fault carried down the pipe becomes real only here, where the
    // instruction is known to be on the committed path
    if (e.isBranch && e.actualTaken && (e.actualTarget & 3)) {
      fprintf(stderr,
              "fatal: misaligned branch target 0x%08x at pc=0x%08x\n",
              e.actualTarget, e.pc);
      exit(1);
    }
    if (e.fault) {
      const char *what = isStore(I.op) ? "store" : "load";
      if (e.fault == 1)
        Memory::failMisaligned(lsq.head().addr, what, e.pc);
      msys.backing.failOutOfRange(lsq.head().addr, what);
    }

    CommitRecord rec;
    rec.sequence = stats.retired;
    rec.pc = e.pc;
    rec.instruction = I.raw;

    if (e.isSystem) {
      switch (I.op) {
      case Op::ECALL: {
        stats.retired++;
        // The argument registers were captured at dispatch into a
        // drained machine, so their values are final
        if (runSyscall(
                prf.val[e.sysA7], prf.val[e.sysA0], e.pc, false,
                [&](uint32_t a) { return msys.peek8(a); }, exitCode,
                msys.backing.bytes.size()))
          halted = true;
        // ECALL executes alone in a drained machine, so updating the current
        // architectural mapping cannot clobber an older or younger operand.
        if (syscallReturnsInput(prf.val[e.sysA7]))
          rec.registerWrite = RegisterWrite{10, prf.val[e.sysA0]};
        break;
      }
      case Op::EBREAK:
        stats.retired++;
        fprintf(stderr, "ebreak at pc=0x%08x; halting\n", e.pc);
        halted = true;
        break;
      case Op::ILLEGAL:
        fprintf(stderr, "illegal instruction 0x%08x at pc=0x%08x\n", I.raw,
                e.pc);
        halted = true;
        exitCode = 1;
        rec.exception = Exception{ExceptionKind::IllegalInstruction};
        break;
      default: // FENCE
        stats.retired++;
        break;
      }
    } else {
      if (e.isMem) {
        LsqEntry &le = lsq.head();
        if (le.isStore) {
          if (sb.full()) { // must drain before this store can retire
            stats.sbCommitStalls++;
            if (trace)
              events.push_back("sb full");
            break;
          }
          sb.push(StoreBufEntry{le.addr, le.size, le.data});
          rec.memoryWrite = MemoryWrite{le.addr, le.data, le.size};
        }
        lsq.popHead();
      }
      if (e.pdst != kNoReg) {
        rec.registerWrite = RegisterWrite{I.rd, prf.val[e.pdst]};
        freeList.push(e.prevPhys); // in-order commit: no older reader is left
      }
      if (e.isBranch) {
        stats.branches++;
        if (e.mispredicted())
          stats.mispredicts++;
        if (cfg.usePredictor)
          pred.update(e.pc, I, e.actualTaken, e.actualTarget, e.predIdx,
                      e.predBefore);
      }
      // A load that commits cleanly slowly re-earns the right to speculate
      if (cfg.depPredictor && e.isMem && isLoad(I.op) && depEntry(e.pc) > 0)
        depEntry(e.pc)--;
      stats.retired++;
    }

    if (trace)
      note(vCT, I);
    if (onCommit)
      onCommit(rec);
    rob.popHead();
    done++;
  }
  // Attribution: nothing committed and the machine's oldest work is a
  // load still waiting for its data (on the cache, an older store, or
  // the data port: this counter lumps them together)
  if (done == 0 && !halted && !rob.empty() && !rob.head().done &&
      rob.head().isMem && isLoad(rob.head().ins.op) && !lsq.empty() &&
      !lsq.head().done)
    stats.memRetireStallCycles++;
}

// Writeback: up to wbPorts results per cycle, oldest first from the
// units' output slots and the load completion slots. Each winner
// writes the physical register file, wakes the issue queue, and marks
// its ROB entry done. A branch is checked against its prediction here,
// the moment its real outcome exists; a wrong prediction triggers
// recovery, which also flushes the rest of this cycle's (younger)
// winners
void OoOCore::writebackStage() {
  auto &cands = wbReady;
  const size_t winners = std::min((size_t)cfg.wbPorts, cands.size());
  for (size_t i = 0; i < winners; i++) {
    // Only winners need ordering. Leave the losing results in their slots.
    const auto oldest = std::min_element(cands.begin(), cands.end(),
        [](const FuOp *a, const FuOp *b) { return a->seq < b->seq; });
    FuOp &op = **oldest;
    *oldest = cands.back();
    cands.pop_back();
    RobEntry &e = rob.at(op.robIdx);
    if (op.pdst != kNoReg) {
      prf.val[op.pdst] = op.value;
      prf.ready[op.pdst] = true;
      iq.wakeup(op.pdst);
    }
    e.done = true;
    if (trace)
      note(vWB, e.ins);
    if (e.isBranch) {
      e.actualTaken = op.redirect;
      e.actualTarget = op.redirect ? op.target : e.pc + 4;
      if (e.mispredicted()) {
        const uint64_t seq = op.seq;
        const uint32_t tgt = e.actualTarget;
        if (cfg.usePredictor) {
          pred.restore(e.predBefore);
          pred.replay(e.pc, e.ins, e.actualTaken);
        }
        op.valid = false; // not younger than seq, so recover() would leave it
        recover(seq);
        pendRedirect = true;
        pendTarget = tgt;
        if (trace) {
          char b[64];
          snprintf(b, sizeof b, "mispredict -> 0x%08x", tgt);
          events.push_back(b);
        }
        break; // younger winners were flushed by recover()
      }
    }
    op.valid = false;
  }
  if (!pendRedirect)
    stats.wbConflicts += cands.size();
}

// Squash everything younger than seq: issue queue, functional units,
// load completion slots, LSQ tail, ROB tail. The ROB walk goes from
// youngest to oldest so each entry restores the mapping it displaced
// and returns its physical register. The fetch queue empties too; the
// pc itself is steered at the clock edge like every redirect
void OoOCore::recover(uint64_t seq) {
  stats.flushes++;
  iq.flushYounger(seq);
  for (FuUnit *u : units)
    u->flushYounger(seq);
  for (FuUnit &u : agus)
    u.flushYounger(seq);
  for (FuOp &l : loadOuts)
    if (l.valid && l.seq > seq)
      l.valid = false;
  wbReady.erase(std::remove_if(wbReady.begin(), wbReady.end(),
      [](const FuOp *op) { return !op->valid; }), wbReady.end());
  // A cache access already in flight for a squashed load cannot be
  // cancelled, and needs no bookkeeping either: its response fails the
  // {slot, generation} tag match and is dropped
  while (lsq.count() && lsq.tail().seq > seq)
    lsq.popTail();
  while (rob.count() && rob.tail().seq > seq) {
    RobEntry &t = rob.tail();
    if (t.pdst != kNoReg) {
      rmap.map[t.ins.rd] = t.prevPhys;
      freeList.push(t.pdst);
    }
    rob.popTail();
  }
  fetchQ.clear();
  // A wrong-path fetch still in flight must be marked stale NOW: it can
  // complete later this same cycle (fetchStage runs after us) and would
  // refill the queue just cleared
  if (fOutstanding)
    fStale = true;
  // Attribution: everything from here until dispatch resumes is the
  // price of this flush (a newer flush restarts the clock)
  recTimerArmed = true;
  recTimerStart = stats.cycles;
}

// The AGU's output feeds the LSQ, not a writeback port: fill in the
// address, record any fault for commit to act on, and mark a store
// done once both its address and data exist. In Speculative mode a
// store address resolving is also the moment ordering violations
// surface
void OoOCore::aguDrain() {
  // Drain oldest first, reselecting after each completion: a store can
  // replay a younger load and invalidate another AGU's pending output.
  for (;;) {
    FuUnit *ready = nullptr;
    for (FuUnit &u : agus)
      if (u.out.valid && (!ready || u.out.seq < ready->out.seq))
        ready = &u;
    if (!ready)
      return;
    const FuOp op = ready->out;
    ready->out.valid = false;
    LsqEntry &le = lsq.at(op.lsqIdx);
    le.addr = op.value;
    le.size = (uint8_t)accessSize(le.op);
    lsq.resolveAddress(le);

    RobEntry &e = rob.at(op.robIdx);
    if (le.addr % le.size != 0)
      e.fault = 1;
    else if ((uint64_t)le.addr + le.size > msys.backing.bytes.size())
      e.fault = 2;
    if (le.isStore) {
      if (le.dataReady)
        e.done = true; // else done when the data operand arrives
      if (cfg.memOrder == MemOrder::Speculative && !e.fault)
        violationScan(le);
    }
  }
}

// A store's address just resolved. Any YOUNGER load that already read
// an overlapping address (from the cache or by forwarding) may have
// consumed a stale value; the oldest such load replays, even if it
// happened to forward the right data. Replay reuses the mispredict
// path wholesale: flush from the load down and refetch it
void OoOCore::violationScan(const LsqEntry &store) {
  const LsqEntry *victim = nullptr;
  lsq.visitAccessedLoads([&](uint32_t slot) {
    const LsqEntry &le = lsq.at(slot);
    if (le.seq > store.seq &&
        overlaps(store.addr, store.size, le.addr, le.size)) {
      victim = &le;
      return false; // oldest overlapping younger load; no need to scan on
    }
    return true;
  });
  if (!victim)
    return;
  stats.loadReplays++;
  if (cfg.depPredictor)
    depEntry(victim->pc) = 3;
  const uint64_t vseq = victim->seq;
  const uint32_t vpc = victim->pc;
  if (trace) {
    char b[64];
    snprintf(b, sizeof b, "load replay @0x%08x", vpc);
    events.push_back(b);
  }
  // Squashed conditionals shifted the speculative history at fetch;
  // rewind to the replaying load's snapshot, just like a mispredict
  if (cfg.usePredictor)
    pred.restore(rob.at(victim->robIdx).predBefore);
  recover(vseq - 1); // the load itself flushes too
  pendRedirect = true;
  pendTarget = vpc;
}

// L1D completions. Store acks carry a stable buffer slot and transaction id.
// Loads match through their {slot, generation} tag; a mismatch means
// the slot was squashed and reused, and the response is dropped
void OoOCore::drainDataResponses() {
  while (dmem->hasResponse()) {
    // Data-port requests are scalar: read only the fields we consume.
    const MemResponse &response = dmem->frontResponse();
    const uint64_t tag = response.tag;
    const uint32_t data = response.rdata;
    dmem->popResponse();
    if (tag & (1ull << 63)) { // a committed store's ack
      const uint32_t txn = (uint32_t)tag;
      const uint32_t slot = uint32_t((tag & ~(1ull << 63)) >> 32);
      if (sb.live(slot)) {
        StoreBufEntry &st = sb.at(slot);
        if (st.inflight && !st.acked && st.txn == txn)
          st.acked = true;
      }
      continue;
    }
    const uint32_t idx = (uint32_t)(tag >> 16);
    const uint16_t gen = (uint16_t)tag;
    if (!lsq.live(idx))
      continue;
    LsqEntry &le = lsq.at(idx);
    if (le.isStore || !le.issued || le.done || le.gen != gen)
      continue; // stale response from a squashed generation
    lsq.completeLoad(le, extendLoad(le.op, data));
  }
}

// The memory engine. Several loads and committed stores can be in
// flight at the L1D at once (its MSHRs absorb the misses); up to
// dataPorts new accesses start per cycle. Which loads may go depends on the
// memory-ordering mode; committed stores stream from the store buffer,
// which wins the port only when full, because a full store buffer
// stalls commit and must always be able to drain
void OoOCore::lsqOperate() {
  drainDataResponses();
  while (!sb.empty() && sb.head().acked)
    sb.popHead(); // acks can arrive out of order; entries pop in order

  // Visit only pending updates, oldest first: surface loads and capture store
  // data whose physical register is ready. Each store's register is
  // written exactly once while the store is live, so captured data stays valid.
  if (lsq.needsUpdate()) {
    size_t nextOutput = 0;
    lsq.visitUpdates([&](LsqEntry &le) {
      if (le.isStore && !le.dataReady && prf.ready[le.dataPreg]) {
        lsq.captureStoreData(le, prf.val[le.dataPreg]);
        if (le.addrValid)
          rob.at(le.robIdx).done = true;
      }
      if (!le.isStore && le.done && !le.reported) {
        while (nextOutput < loadOuts.size() && loadOuts[nextOutput].valid)
          nextOutput++;
        if (nextOutput == loadOuts.size())
          return true;
        FuOp &slot = loadOuts[nextOutput++];
        slot.valid = true;
        slot.seq = le.seq;
        slot.robIdx = le.robIdx;
        slot.pdst = le.pdst;
        slot.value = le.value;
        wbReady.push_back(&slot);
        lsq.reportLoad(le);
      }
      return true;
    });
  }

  // A shared load/store bandwidth budget. Responses and completed-load
  // reporting stay outside this loop, so more ports do not shorten latency.
  for (uint32_t access = 0; access < cfg.dataPorts; ++access) {
    bool sent = false;
    // Visit only address-ready, unissued loads, in age order. An older
    // store with an unknown address is a wall in
    // Conservative and Bypass modes (and, in Speculative mode, for loads
    // the dependence predictor has flagged); other Speculative loads walk
    // straight past it and pay with a replay when wrong
    LsqEntry *cand = nullptr;
    uint32_t candRing = 0;
    bool candSpeculative = false;
    const uint64_t unknownSeq = lsq.oldestUnknownStore();
    const uint64_t pendingSeq = cfg.memOrder == MemOrder::Conservative
                                    ? lsq.oldestPendingLoad() : UINT64_MAX;
    lsq.visitCandidates([&](uint32_t slot) {
      LsqEntry &le = lsq.at(slot);
      if (le.seq > pendingSeq)
        return false; // conservative mode: no passing an in-flight load
      const bool wall = unknownSeq < le.seq;
      if (rob.at(le.robIdx).fault) {
        // Faulted (possibly wrong-path garbage): never touch memory.
        // Produce a zero so the entry can reach commit, which decides
        // whether the fault is real
        lsq.completeLoad(le, 0);
        return true;
      }
      const bool cautious = cfg.memOrder != MemOrder::Speculative ||
                            (cfg.depPredictor && depEntry(le.pc) >= 2);
      if (wall && cautious)
        return true;
      // Visit known older stores from the newest, then the
      // store buffer (all committed, hence older). The nearest older store
      // to a matching address decides: an exact match forwards, a partial
      // overlap waits (no byte merging across stores)
      bool blocked = false, forwarded = false;
      auto olderStore = [&](uint32_t addr, uint32_t size, bool dataReady,
                            uint32_t data) {
        if (addr == le.addr && size == le.size) {
          if (!dataReady) {
            blocked = true; // aliases, but the data doesn't exist yet
          } else {
            lsq.completeLoad(le, extendLoad(le.op, data));
            forwarded = true;
          }
        } else if (overlaps(addr, size, le.addr, le.size)) {
          blocked = true; // partial overlap: wait for the store to drain
        }
      };
      if (lsq.mayStoreToWord(le.addr))
        lsq.visitOlderStores(slot, [&](const LsqEntry &st) {
          olderStore(st.addr, st.size, st.dataReady, st.data);
          return !blocked && !forwarded;
        });
      for (int j = (int)sb.count() - 1; j >= 0 && !blocked && !forwarded; j--) {
        const StoreBufEntry &st = sb.nth((uint32_t)j);
        olderStore(st.addr, st.size, true, st.data);
      }
      if (forwarded) {
        stats.loadsForwarded++;
        if (wall)
          stats.specLoads++; // forwarded past an unknown address
        if (trace)
          events.push_back("store->load forward");
        if (cfg.memOrder == MemOrder::Conservative)
          return false;
        return true; // forwarding is free; keep looking for a port customer
      }
      if (!blocked) {
        cand = &le;
        candRing = slot;
        candSpeculative = wall;
        return false;
      }
      // blocked: Conservative stays in order; the other modes may issue a
      // younger, independent load instead
      if (cfg.memOrder == MemOrder::Conservative)
        return false;
      return true;
    });

    if (cand || !sb.empty()) {
      // For each available access, the store buffer goes first only
      // when full or when no load is ready
      StoreBufEntry *st = sb.nextToIssue();
      if (st && (sb.full() || !cand)) {
        if (dmem->canAccept()) {
          MemRequest req;
          req.addr = st->addr;
          req.size = st->size;
          req.isWrite = true;
          req.wdata = st->data;
          req.tag = (1ull << 63) | (uint64_t(sb.nextIssueSlot()) << 32) | sbTxnCtr;
          st->txn = sbTxnCtr++;
          sb.issue();
          dmem->access(req);
          sent = true;
        }
      } else if (cand && dmem->canAccept()) {
        MemRequest req;
        req.addr = cand->addr;
        req.size = cand->size;
        req.tag = ((uint64_t)candRing << 16) | cand->gen;
        lsq.issueLoad(*cand);
        if (candSpeculative)
          stats.specLoads++;
        dmem->access(req);
        sent = true;
      }
    }
    if (!sent)
      break;
  }

  drainDataResponses(); // a combinational hit completes this cycle

  if (lsq.hasPendingLoads()) {
    stats.dataStallCycles++;
    if (trace)
      events.push_back("dmem wait");
  }
}

// Issue: up to width instructions per cycle leave the issue queue,
// oldest first among those whose operands are ready and whose
// functional unit can accept. Operand values come from the physical
// register file; writeback ran earlier this cycle, so a ready bit set
// today is readable today
void OoOCore::issueStage() {
  uint32_t issued = 0;
  size_t nextAlu = 0, nextAgu = 0;
  iq.visitReady(rob.indexOf(0), [&](IqEntry &candidate) {
    IqEntry *q = &candidate;
    const RobEntry &entry = rob.at(q->robIdx);
    const Instr &I = entry.ins;
    FuUnit *unit = nullptr;
    switch (q->unit) {
    case FuKind::ALU:
      // Units only get busier during issue, so each search resumes
      // where the previous one stopped
      while (nextAlu < alus.size() && !alus[nextAlu].canAccept())
        nextAlu++;
      if (nextAlu < alus.size())
        unit = &alus[nextAlu++];
      break;
    case FuKind::BRANCH:
      unit = &brUnit;
      break;
    case FuKind::MUL:
      unit = &mulUnit;
      break;
    case FuKind::DIV:
      unit = &divUnit;
      break;
    case FuKind::LSU:
      while (nextAgu < agus.size() && !agus[nextAgu].canAccept())
        nextAgu++;
      if (nextAgu < agus.size())
        unit = &agus[nextAgu++];
      break;
    case FuKind::NONE:
      break; // system ops never enter the issue queue
    }
    if (!unit || !unit->canAccept())
      return true; // the unit is busy; try a younger ready op instead

    // execute() ignores operands the instruction doesn't use
    const ExecResult r = execute(I, entry.pc, prf.val[q->ps1], prf.val[q->ps2]);

    FuOp op;
    op.valid = true;
    op.seq = q->seq;
    op.robIdx = q->robIdx;
    op.lsqIdx = q->lsqIdx;
    op.pdst = entry.pdst;
    op.value = r.value;
    op.redirect = r.redirect;
    op.target = r.target;
    unit->accept(op);
    if (trace)
      note(vIS, I);
    iq.release(q);
    issued++;
    stats.issuedOps++;
    return issued < cfg.width;
  });
}

// Dispatch: up to width per cycle, in program order: decode, rename,
// allocate ROB (and LSQ) entries, drop into the issue queue. Any
// missing resource stalls this instruction and everything younger
void OoOCore::dispatchStage() {
  for (uint32_t w = 0; w < cfg.width; w++) {
    if (fetchQ.empty()) {
      if (w == 0)
        stats.dsFetchEmpty++;
      return;
    }
    const Fetched f = fetchQ.head();
    const Instr I = decode(f.raw);
    const FuKind unit = fuKindOf(I.op);
    const bool sys = unit == FuKind::NONE;
    const bool mem = isLoad(I.op) || isStore(I.op);
    const bool hasDest = writesRd(I.op) && I.rd != 0;

    // System ops wait for the machine to empty completely (all older
    // work committed AND the store buffer drained), then run alone
    // and take effect at commit
    if (sys && (w != 0 || !rob.empty() || !sb.empty())) {
      if (w == 0) {
        stats.dsSerialize++;
        if (trace)
          events.push_back("serialize wait");
      }
      return;
    }
    if (rob.full()) {
      if (w == 0)
        stats.dsRobFull++;
      return;
    }
    if (mem && lsq.full()) {
      if (w == 0)
        stats.dsLsqFull++;
      return;
    }
    if (iq.full()) {
      if (w == 0)
        stats.dsIqFull++;
      return;
    }
    if (hasDest && freeList.empty()) {
      if (w == 0)
        stats.dsNoPreg++; // only reachable with an explicit, smaller physRegs
      return;
    }

    if (recTimerArmed) { // dispatch resumed: close the flush window
      stats.recoveryLossCycles += stats.cycles - recTimerStart;
      recTimerArmed = false;
    }
    fetchQ.popHead();
    const uint64_t seq = seqCtr++;
    const uint32_t robIdx = rob.alloc();
    RobEntry &e = rob.at(robIdx);
    e.seq = seq;
    e.pc = f.pc;
    e.ins = I;
    // Every entry keeps its fetch-time history snapshot: a flush from
    // ANY point (branch mispredict or load replay) must be able to
    // rewind the speculative history and RAS to before the squashed ones
    e.predBefore = f.predBefore;

    if (sys) {
      e.done = true;
      e.isSystem = true;
      e.sysA0 = rmap.map[10]; // the map is architectural right now
      e.sysA7 = rmap.map[17];
      if (trace)
        note(vDS, I);
      return;
    }

    // Sources rename through the CURRENT map, including updates made
    // by the older instruction dispatched this same cycle, and before
    // the destination displaces anything (rs may equal rd)
    const uint8_t ps1 = rmap.map[I.rs1];
    const uint8_t ps2 = rmap.map[I.rs2];
    if (hasDest) {
      e.pdst = freeList.head();
      freeList.popHead();
      e.prevPhys = rmap.map[I.rd];
      rmap.map[I.rd] = e.pdst;
      prf.ready[e.pdst] = false;
    }

    e.isMem = mem;
    uint32_t lsqIdx = 0;
    if (mem) {
      lsqIdx = lsq.alloc();
      LsqEntry &le = lsq.at(lsqIdx);
      le.seq = seq;
      le.robIdx = robIdx;
      le.pc = f.pc;
      le.op = I.op;
      le.isStore = isStore(I.op);
      le.pdst = e.pdst;
      if (le.isStore) {
        // Address and data resolve independently: capture the data
        // now if its producer already wrote back, else watch its preg
        lsq.setStoreSource(le, ps2, prf.ready[ps2], prf.val[ps2]);
      }
    }
    if (unit == FuKind::BRANCH) {
      e.isBranch = true;
      e.predictedTaken = f.predTaken;
      e.predictedTarget = f.predTarget;
      e.predIdx = f.predIdx;
    }

    IqEntry *q = iq.allocate(robIdx);
    q->seq = seq;
    q->lsqIdx = lsqIdx;
    q->unit = unit;
    q->ps1 = ps1;
    q->ps2 = ps2;
    // Stores don't wait for their data to issue; only the address
    // operand gates them, and the LSQ captures the data when it
    // appears
    iq.track(q, !usesRs1(I.op) || prf.ready[ps1],
             !usesRs2(I.op) || isStore(I.op) || prf.ready[ps2]);
    if (trace)
      note(vDS, I);
  }
}

// Fetch: one aligned block per cycle, steered by the predictor. pc
// advances when the block arrives (the predictor may redirect it
// mid-block); a squash marks the in-flight fetch stale
void OoOCore::fetchStage() {
  auto consume = [&] {
    const MemResponse &r = imem->frontResponse();
    fOutstanding = false;
    if (fStale)
      fStale = false;
    else
      processFetch(r);
    imem->popResponse();
  };
  if (fOutstanding && imem->hasResponse())
    consume();
  if (fOutstanding || pendRedirect ||
      fetchQ.count() + fBytes / 4 > cfg.fetchQSize)
    return;
  if (pc % 4 != 0) {
    // A younger branch may redirect here before an older branch squashes
    // it. Wait for recovery; an unsquashed bad target faults at commit.
    stats.badFetchWaitCycles++;
    return;
  }
  if (pc >= msys.backing.bytes.size()) {
    // The same wait for a target outside memory. Once nothing older is
    // in flight, pc is architectural and the fetch fault is real
    if (rob.empty() && fetchQ.count() == 0)
      msys.backing.failOutOfRange(pc, "instruction fetch");
    stats.badFetchWaitCycles++;
    return;
  }
  if (!imem->canAccept())
    return;
  MemRequest req;
  req.addr = pc & ~(fBytes - 1);
  req.size = fBytes;
  imem->access(req);
  fOutstanding = true;
  fBlock = req.addr;
  fFetchPc = pc;
  if (imem->hasResponse()) // 1-cycle port: complete this cycle
    consume();
  else if (trace)
    events.push_back("ifetch wait");
}

void OoOCore::processFetch(const MemResponse &r) {
  for (uint32_t idx = (fFetchPc - fBlock) / 4; idx < fBytes / 4; idx++) {
    const uint32_t raw = (uint32_t)r.rline[idx * 4] |
                         (uint32_t)r.rline[idx * 4 + 1] << 8 |
                         (uint32_t)r.rline[idx * 4 + 2] << 16 |
                         (uint32_t)r.rline[idx * 4 + 3] << 24;
    const uint32_t wpc = fBlock + idx * 4;
    Predictor::Pred p;
    if (cfg.usePredictor)
      p = pred.predict(wpc);
    fetchQ.push(
        Fetched{wpc, raw, p.taken, p.target, p.phtIdx, p.before});
    if (p.taken) {
      pc = p.target;
      return;
    }
  }
  pc = fBlock + fBytes;
}

// One clock cycle: stages in reverse pipeline order (each consumes
// what the stage behind it produced in an earlier cycle), redirect at
// the clock edge, units and memory ticking last
void OoOCore::stepCycle() {
  stats.cycles++;
  // start-of-cycle occupancy samples, averaged in the final report
  stats.robOccSum += rob.count();
  stats.iqOccSum += iq.count();
  stats.lsqOccSum += lsq.count();
  stats.sbOccSum += sb.count();
  stats.pregsUsedSum += (cfg.physRegs - 32) - freeList.count();
  if (trace) {
    events.clear();
    vDS.clear();
    vIS.clear();
    vWB.clear();
    vCT.clear();
    captureView();
  }

  commitStage();
  if (halted) {
    if (trace)
      printTrace();
    return;
  }
  writebackStage();
  aguDrain();
  lsqOperate();
  issueStage();
  dispatchStage();
  fetchStage();

  if (pendRedirect) {
    if (fOutstanding)
      fStale = true;
    pc = pendTarget;
    pendRedirect = false;
  }

  for (FuUnit *u : units)
    if (u->tick())
      wbReady.push_back(&u->out);
  for (FuUnit &u : agus)
    u.tick(); // address completions never consume a writeback port
  msys.tick();
  if (trace)
    printTrace();
}

// Trace: start-of-cycle occupancy, then per-stage activity as it
// happens, then events

void OoOCore::note(std::string &v, const Instr &ins) {
  if (!trace)
    return;
  if (!v.empty())
    v += "; ";
  v += disasm(ins);
}

void OoOCore::captureView() {
  char b[64];
  snprintf(b, sizeof b, "fq%u rob%2u iq%2u lsq%2u sb%u", fetchQ.count(),
           rob.count(), iq.count(), lsq.count(), sb.count());
  vOcc = b;
}

void OoOCore::printTrace() const {
  fprintf(stderr,
          "cyc %6" PRIu64 " | %s | DS %-28s | IS %-28s | WB %-28s | CT %-28s",
          stats.cycles, vOcc.c_str(), vDS.empty() ? "-" : vDS.c_str(),
          vIS.empty() ? "-" : vIS.c_str(), vWB.empty() ? "-" : vWB.c_str(),
          vCT.empty() ? "-" : vCT.c_str());
  for (const auto &event : events)
    fprintf(stderr, "  ! %s", event.c_str());
  fputc('\n', stderr);
}
