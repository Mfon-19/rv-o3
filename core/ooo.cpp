#include "core/ooo.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isa/execute.h"

OoOCore::OoOCore(const SimConfig &cfg, MemorySystem &msys)
    : cfg(cfg), msys(msys), imem(msys.imem), dmem(msys.dmem),
      showMemStats(!cfg.flatMemory || cfg.flatLatency > 1),
      freeList(cfg.physRegs), prf(cfg.physRegs), rob(cfg.robSize),
      iq(cfg.iqSize), lsq(cfg.lsqSize), sb(cfg.sbSize), pred(cfg),
      alus(cfg.aluCount, FuUnit("alu", 1, true)), brUnit("br", 1, true),
      mulUnit("mul", cfg.mulLatency, cfg.mulPipelined),
      divUnit("div", cfg.divLatency, false), agu("agu", 1, true),
      loadOuts(cfg.width > 2 ? cfg.width : 2), depTable(cfg.depTableSize, 0),
      trace(cfg.trace) {
  fBytes = cfg.width * 4 > 8 ? cfg.width * 4 : 8;
  rmap.reset(); // arch reg i starts in physical reg i...
  for (uint32_t p = 32; p < cfg.physRegs; p++)
    freeList.push((uint8_t)p); // ...the rest are free
}

void OoOCore::loadWords(const std::vector<uint32_t> &words) {
  for (size_t i = 0; i < words.size(); i++)
    msys.backing.store32((uint32_t)(i * 4), words[i]);
}

void OoOCore::loadBytes(const std::vector<uint8_t> &bytes) {
  msys.backing.check(0, (uint32_t)bytes.size(), "program image");
  memcpy(msys.backing.bytes.data(), bytes.data(), bytes.size());
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
  fprintf(stderr, "--- rvsim: %" PRIu64 " cycles, %" PRIu64
          " instructions retired, IPC = %.3f (CPI = %.3f)\n",
          stats.cycles, stats.retired,
          stats.cycles ? (double)stats.retired / (double)stats.cycles : 0.0,
          stats.retired ? (double)stats.cycles / (double)stats.retired : 0.0);
  fprintf(stderr, "--- rvsim: dispatch stalls: rob-full %" PRIu64
          ", iq-full %" PRIu64 ", lsq-full %" PRIu64 ", no-preg %" PRIu64
          ", serialize %" PRIu64 ", fetch-empty %" PRIu64 "\n",
          stats.dsRobFull, stats.dsIqFull, stats.dsLsqFull, stats.dsNoPreg,
          stats.dsSerialize, stats.dsFetchEmpty);
  fprintf(stderr, "--- rvsim: %" PRIu64 " branches, %" PRIu64
          " mispredicted (%.1f%%, %.2f MPKI), %" PRIu64
          " flushes; avg issue %.2f; wb-port holds %" PRIu64 "\n",
          stats.branches, stats.mispredicts,
          stats.branches ? 100.0 * (double)stats.mispredicts /
                               (double)stats.branches
                         : 0.0,
          stats.retired ? 1000.0 * (double)stats.mispredicts /
                              (double)stats.retired
                        : 0.0,
          stats.flushes,
          stats.cycles ? (double)stats.issuedOps / (double)stats.cycles : 0.0,
          stats.wbConflicts);
  fprintf(stderr, "--- rvsim: %" PRIu64 " dispatched, %" PRIu64
          " wrong-path (%.1f%%); recovery loss %" PRIu64
          " cycles; retire blocked on loads %" PRIu64 " cycles\n",
          seqCtr, seqCtr - stats.retired,
          seqCtr ? 100.0 * (double)(seqCtr - stats.retired) / (double)seqCtr
                 : 0.0,
          stats.recoveryLossCycles, stats.memRetireStallCycles);
  const double occDiv = stats.cycles ? (double)stats.cycles : 1.0;
  fprintf(stderr, "--- rvsim: occupancy: rob %.1f/%u, iq %.1f/%u, "
          "lsq %.1f/%u, sb %.1f/%u, pregs %.1f/%u\n",
          (double)stats.robOccSum / occDiv, cfg.robSize,
          (double)stats.iqOccSum / occDiv, cfg.iqSize,
          (double)stats.lsqOccSum / occDiv, cfg.lsqSize,
          (double)stats.sbOccSum / occDiv, cfg.sbSize,
          (double)stats.pregsUsedSum / occDiv, cfg.physRegs - 32);
  fprintf(stderr, "--- rvsim: fu busy:");
  auto fuLine = [&](const char *name, uint64_t busy, uint64_t ops) {
    fprintf(stderr, " %s %.1f%% (%" PRIu64 " ops)", name,
            stats.cycles ? 100.0 * (double)busy / (double)stats.cycles : 0.0,
            ops);
  };
  uint64_t aluBusy = 0, aluOps = 0;
  for (const FuUnit &u : alus) {
    aluBusy += u.busyCycles;
    aluOps += u.ops;
  }
  fuLine("alu", aluBusy, aluOps);
  fuLine("br", brUnit.busyCycles, brUnit.ops);
  fuLine("mul", mulUnit.busyCycles, mulUnit.ops);
  fuLine("div", divUnit.busyCycles, divUnit.ops);
  fuLine("agu", agu.busyCycles, agu.ops);
  fputc('\n', stderr);
  fprintf(stderr, "--- rvsim: %" PRIu64 " loads forwarded from stores, %"
          PRIu64 " speculative loads, %" PRIu64 " replays, %" PRIu64
          " store-buffer commit stalls\n",
          stats.loadsForwarded, stats.specLoads, stats.loadReplays,
          stats.sbCommitStalls);
  if (showMemStats) {
    stats.fetchStallCycles = stats.dsFetchEmpty;
    stats.printMemStalls();
    msys.printStats(stats.retired);
  }
  stats.printExit(exitCode);
  return exitCode;
}

void OoOCore::dumpRegs() const {
  fprintf(stderr, "--- registers ---\n");
  for (int i = 0; i < 32; i++) {
    fprintf(stderr, "%4s=%08x%s", kRegName[i], reg(i),
            (i % 4 == 3) ? "\n" : "  ");
  }
  fprintf(stderr, "pc  =%08x\n", pc);
}

// Commit: up to width instructions per cycle from the ROB head, in
// program order; this is the only place architectural state changes
void OoOCore::commitStage() {
  uint32_t done = 0;
  while (done < cfg.width && !rob.empty() && rob.head().done && !halted) {
    RobEntry &e = rob.head();
    const Instr &I = e.ins;

    // A fault carried down the pipe becomes real only here, where the
    // instruction is known to be on the committed path. Mirror the
    // reference model's fatal messages exactly
    if (e.fault) {
      const uint32_t addr = e.isMem ? lsq.head().addr : 0;
      if (e.fault == 1)
        fprintf(stderr, "fatal: misaligned %s of 0x%08x at pc=0x%08x\n",
                isStore(I.op) ? "store" : "load", addr, e.pc);
      else
        fprintf(stderr, "fatal: %s at 0x%08x is outside memory (%zu bytes)\n",
                isStore(I.op) ? "store" : "load", addr,
                msys.backing.bytes.size());
      exit(1);
    }

    CommitRecord rec;
    rec.sequence = stats.retired;
    rec.pc = e.pc;
    rec.instruction = I.raw;

    if (e.isSystem) {
      switch (I.op) {
      case Op::ECALL:
        stats.retired++;
        doSyscall(e);
        break;
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
      note(vCT, I);
      if (onCommit)
        onCommit(rec);
      rob.popHead();
      done++;
      continue;
    }

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
        const uint32_t v = (le.size == 4)
                               ? le.data
                               : (le.data & ((1u << (8 * le.size)) - 1));
        rec.memoryWrite = MemoryWrite{le.addr, v, le.size};
      }
      lsq.popHead();
    }

    if (e.pdst != kNoReg) {
      rec.registerWrite = RegisterWrite{I.rd, prf.val[e.pdst]};
      freeList.push(e.prevPhys); // no older reader can be in flight now
    }
    if (e.isBranch) {
      stats.branches++;
      if (e.actualTaken != e.predictedTaken ||
          (e.actualTaken && e.actualTarget != e.predictedTarget))
        stats.mispredicts++;
      if (cfg.usePredictor)
        pred.update(e.pc, I, e.actualTaken, e.actualTarget, e.predIdx);
    }
    // A load committing cleanly slowly re-earns the right to speculate
    if (cfg.depPredictor && e.isMem && isLoad(I.op)) {
      uint8_t &c = depTable[(e.pc >> 2) % depTable.size()];
      if (c > 0)
        c--;
    }
    stats.retired++;
    note(vCT, I);
    if (onCommit)
      onCommit(rec);
    rob.popHead();
    done++;
  }
  // Attribution: nothing committed and the machine's oldest work is a
  // load still waiting on the cache, so this cycle is memory's fault
  if (done == 0 && !halted && !rob.empty() && !rob.head().done &&
      rob.head().isMem && isLoad(rob.head().ins.op) && !lsq.empty() &&
      !lsq.head().done)
    stats.memRetireStallCycles++;
}

// Writeback: up to wbPorts results per cycle, taken oldest first from
// the units' output slots plus the load completion slots. Each winner
// writes the physical register file, announces its register so
// instructions waiting on it become ready, and marks its ROB entry
// done. A branch is checked against its prediction here, the moment
// its real outcome exists; a wrong prediction triggers recovery,
// which also flushes the rest of this cycle's (younger) writebacks
void OoOCore::writebackStage() {
  std::vector<FuOp *> cands;
  for (FuUnit &u : alus)
    if (u.out.valid)
      cands.push_back(&u.out);
  for (FuUnit *u : {&brUnit, &mulUnit, &divUnit})
    if (u->out.valid)
      cands.push_back(&u->out);
  for (FuOp &l : loadOuts)
    if (l.valid)
      cands.push_back(&l);
  std::sort(cands.begin(), cands.end(),
            [](const FuOp *a, const FuOp *b) { return a->seq < b->seq; });

  const size_t winners = std::min((size_t)cfg.wbPorts, cands.size());
  size_t i = 0;
  for (; i < winners; i++) {
    FuOp &op = *cands[i];
    RobEntry &e = rob.at(op.robIdx);
    if (op.pdst != kNoReg) {
      prf.val[op.pdst] = op.value;
      prf.ready[op.pdst] = true;
      iq.wakeup(op.pdst);
    }
    e.done = true;
    note(vWB, op.ins);
    if (e.isBranch) {
      e.actualTaken = op.redirect;
      e.actualTarget = op.redirect ? op.target : op.pc + 4;
      const bool misp =
          e.actualTaken != e.predictedTaken ||
          (e.actualTaken && e.actualTarget != e.predictedTarget);
      if (misp) {
        const uint64_t seq = op.seq;
        const uint32_t tgt = e.actualTarget;
        if (cfg.usePredictor)
          pred.restore(e.ghrBefore, isBranch(op.ins.op), e.actualTaken);
        op = FuOp{};
        recover(seq);
        pendRedirect = true;
        pendTarget = tgt;
        if (trace) {
          char b[64];
          snprintf(b, sizeof b, "mispredict -> 0x%08x", tgt);
          events.push_back(b);
        }
        i++;
        break; // younger winners were flushed by recover()
      }
    }
    op = FuOp{};
  }
  if (cands.size() > winners && !pendRedirect)
    stats.wbConflicts += cands.size() - winners;
}

// Squash everything younger than seq: issue queue, functional units,
// loads in flight, LSQ tail, and the ROB tail. The walk goes from
// youngest to oldest so each entry restores the mapping it displaced
// and returns its physical register. The frontend queue empties too; the pc itself is
// steered at the clock edge like every redirect
void OoOCore::recover(uint64_t seq) {
  stats.flushes++;
  iq.flushYounger(seq);
  for (FuUnit &u : alus)
    u.flushYounger(seq);
  brUnit.flushYounger(seq);
  mulUnit.flushYounger(seq);
  divUnit.flushYounger(seq);
  agu.flushYounger(seq);
  for (FuOp &l : loadOuts)
    if (l.valid && l.seq > seq)
      l = FuOp{};
  // In-flight cache accesses of squashed loads need no bookkeeping:
  // their responses fail the {slot, generation} tag match and die
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
  // A wrong-path fetch still in flight must be marked stale NOW: it
  // can complete later this same cycle (fetchStage runs after us) and
  // would land wrong-path words in the queue we just cleared
  if (fOutstanding)
    fStale = true;
  // Attribution: everything from here until dispatch resumes is the
  // price of this flush (a newer flush restarts the clock)
  recTimerArmed = true;
  recTimerStart = stats.cycles;
}

// The AGU's output feeds the LSQ, not a writeback port: fill in the
// address, record any fault for commit to act on, and mark stores
// complete once both address and data exist. In Speculative mode a
// store address resolving is also the moment ordering violations
// surface: any younger load that already executed against this
// address guessed wrong
void OoOCore::aguDrain() {
  if (!agu.out.valid)
    return;
  FuOp op = agu.out;
  agu.out = FuOp{};
  LsqEntry &le = lsq.at(op.lsqIdx);
  le.addr = op.value;
  le.size = (uint8_t)accessSize(op.ins.op);
  le.addrValid = true;

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

// A store's address just resolved: any YOUNGER load that already read
// (from the cache or by forwarding) an overlapping address consumed a
// stale value. Replay the oldest such load by reusing the mispredict
// path wholesale: flush from the load down and refetch it
void OoOCore::violationScan(const LsqEntry &store) {
  const LsqEntry *victim = nullptr;
  for (uint32_t k = 0; k < lsq.count(); k++) {
    LsqEntry &le = lsq.nth(k);
    if (le.isStore || le.seq <= store.seq || !le.addrValid)
      continue;
    if (!le.issued && !le.done)
      continue; // hasn't touched memory yet: still safe
    if (store.addr + store.size > le.addr && le.addr + le.size > store.addr)
      if (!victim || le.seq < victim->seq)
        victim = &le;
  }
  if (!victim)
    return;
  stats.loadReplays++;
  if (cfg.depPredictor)
    depTable[(victim->pc >> 2) % depTable.size()] = 3;
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
    pred.restore(rob.at(victim->robIdx).ghrBefore, false, false);
  recover(vseq - 1); // the load itself flushes too
  pendRedirect = true;
  pendTarget = vpc;
}

// L1D completions. Loads match through their {slot, generation} tag;
// a mismatch means the load was squashed and the response just dies.
// Store acknowledgements mark their store-buffer entry
void OoOCore::drainDataResponses() {
  while (dmem->hasResponse()) {
    MemResponse r = dmem->response();
    if (r.tag & (1ull << 63)) { // a committed store's ack
      const uint32_t txn = (uint32_t)r.tag;
      for (uint32_t k = 0; k < sb.count(); k++) {
        StoreBufEntry &st = sb.nth(k);
        if (st.inflight && !st.acked && st.txn == txn) {
          st.acked = true;
          break;
        }
      }
      continue;
    }
    const uint32_t idx = (uint32_t)(r.tag >> 16);
    const uint16_t gen = (uint16_t)r.tag;
    if (!lsq.live(idx))
      continue;
    LsqEntry &le = lsq.at(idx);
    if (le.isStore || !le.issued || le.done || le.gen != gen)
      continue; // stale response from a squashed generation
    le.value = extendLoad(le.ins.op, r.rdata);
    le.done = true;
  }
}

// The memory engine. Several loads and committed stores can be in
// flight at the L1D at once (the cache's MSHRs absorb the misses);
// one new access starts per cycle. Which loads may go depends on the
// memory-ordering mode; committed stores stream from the store
// buffer, which gets priority only when full, because a full store
// buffer stalls commit and must always be able to drain
void OoOCore::lsqOperate() {
  drainDataResponses();
  while (!sb.empty() && sb.head().acked)
    sb.popHead(); // pop in order once the cache applied them

  // Surface completed loads to the writeback arbiter, oldest first
  for (FuOp &slot : loadOuts) {
    if (slot.valid)
      continue;
    LsqEntry *best = nullptr;
    for (uint32_t k = 0; k < lsq.count(); k++) {
      LsqEntry &le = lsq.nth(k);
      if (!le.isStore && le.done && !le.reported &&
          (!best || le.seq < best->seq))
        best = &le;
    }
    if (!best)
      break;
    slot.valid = true;
    slot.seq = best->seq;
    slot.robIdx = best->robIdx;
    slot.ins = best->ins;
    slot.pc = best->pc;
    slot.pdst = best->pdst;
    slot.value = best->value;
    best->reported = true;
  }

  // Split stores: capture the data operand the moment its physical
  // register is ready (it is written exactly once, so never stale)
  for (uint32_t k = 0; k < lsq.count(); k++) {
    LsqEntry &le = lsq.nth(k);
    if (le.isStore && !le.dataReady && prf.ready[le.dataPreg]) {
      le.data = prf.val[le.dataPreg];
      le.dataReady = true;
      if (le.addrValid)
        rob.at(le.robIdx).done = true;
    }
  }

  // Find the oldest load allowed to execute this cycle. Walking from
  // the oldest, an older store with an unknown address is a wall in
  // Conservative and Bypass modes (and, in Speculative mode, for
  // loads the dependence predictor has flagged); Speculative mode
  // walks straight past it and pays with a replay when wrong
  LsqEntry *cand = nullptr;
  uint32_t candRing = 0;
  bool wall = false; // an older store address is still unknown
  bool candSpeculative = false;
  for (uint32_t k = 0; k < lsq.count(); k++) {
    LsqEntry &le = lsq.nth(k);
    if (le.isStore) {
      if (!le.addrValid)
        wall = true;
      continue;
    }
    if (le.issued && !le.done) {
      if (cfg.memOrder == MemOrder::Conservative)
        break; // serial loads: one at a time, in order
      continue;
    }
    if (!le.addrValid || le.done)
      continue;
    if (rob.at(le.robIdx).fault) {
      // Faulted (possibly wrong-path garbage): never touch memory;
      // produce zero and let commit decide whether it is fatal
      le.value = 0;
      le.done = true;
      continue;
    }
    const bool cautious =
        cfg.memOrder != MemOrder::Speculative ||
        (cfg.depPredictor && depTable[(le.pc >> 2) % depTable.size()] >= 2);
    if (wall && cautious)
      continue;
    // Scan older stores from the newest: LSQ entries before k, then the
    // store buffer (all committed, hence older). The nearest older
    // store to a matching address decides; a partial overlap waits
    bool blocked = false, forwarded = false;
    for (int j = (int)k - 1; j >= 0 && !blocked && !forwarded; j--) {
      LsqEntry &st = lsq.nth((uint32_t)j);
      if (!st.isStore || !st.addrValid)
        continue; // unknown address: only reachable when speculating
      if (st.addr == le.addr && st.size == le.size) {
        if (!st.dataReady) {
          blocked = true; // aliases, but the data doesn't exist yet
        } else {
          le.value = extendLoad(le.ins.op, st.data);
          le.done = true;
          forwarded = true;
        }
      } else if (st.addr + st.size > le.addr && le.addr + le.size > st.addr) {
        blocked = true; // partial overlap: wait for the store to drain
      }
    }
    for (int j = (int)sb.count() - 1; j >= 0 && !blocked && !forwarded; j--) {
      StoreBufEntry &st = sb.nth((uint32_t)j);
      if (st.addr == le.addr && st.size == le.size) {
        le.value = extendLoad(le.ins.op, st.data);
        le.done = true;
        forwarded = true;
      } else if (st.addr + st.size > le.addr && le.addr + le.size > st.addr) {
        blocked = true;
      }
    }
    if (forwarded) {
      stats.loadsForwarded++;
      if (wall)
        stats.specLoads++; // forwarded past an unknown address
      if (trace)
        events.push_back("store->load forward");
      if (cfg.memOrder == MemOrder::Conservative)
        break;
      continue; // forwarding is free; keep looking for a port customer
    }
    if (!blocked) {
      cand = &le;
      candRing = lsq.indexOf(k);
      candSpeculative = wall;
      break;
    }
    // blocked: in-order below Bypass, else younger loads may proceed
    if (cfg.memOrder == MemOrder::Conservative)
      break;
  }

  if (cand || !sb.empty()) {
    // One new L1D access per cycle: the store buffer goes first only
    // when full, else the load
    StoreBufEntry *st = nullptr;
    for (uint32_t k = 0; k < sb.count(); k++)
      if (!sb.nth(k).inflight) {
        st = &sb.nth(k); // oldest not-yet-issued store
        break;
      }
    if (st && (sb.full() || !cand)) {
      if (dmem->canAccept()) {
        MemRequest req;
        req.addr = st->addr;
        req.size = st->size;
        req.isWrite = true;
        req.wdata = st->data;
        req.tag = (1ull << 63) | sbTxnCtr;
        st->txn = sbTxnCtr++;
        st->inflight = true;
        dmem->access(req);
      }
    } else if (cand && dmem->canAccept()) {
      MemRequest req;
      req.addr = cand->addr;
      req.size = cand->size;
      req.tag = ((uint64_t)candRing << 16) | cand->gen;
      cand->issued = true;
      if (candSpeculative)
        stats.specLoads++;
      dmem->access(req);
    }
  }

  drainDataResponses(); // a combinational hit completes this cycle

  bool waiting = false;
  for (uint32_t k = 0; k < lsq.count(); k++)
    if (!lsq.nth(k).isStore && lsq.nth(k).issued && !lsq.nth(k).done)
      waiting = true;
  if (waiting) {
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
  std::vector<IqEntry *> ready;
  for (IqEntry &q : iq.e)
    if (q.valid && q.ready1 && q.ready2)
      ready.push_back(&q);
  std::sort(ready.begin(), ready.end(),
            [](const IqEntry *a, const IqEntry *b) { return a->seq < b->seq; });

  uint32_t issued = 0;
  for (IqEntry *q : ready) {
    if (issued == cfg.width)
      break;
    const Instr &I = q->ins;
    FuUnit *unit = nullptr;
    switch (fuKindOf(I.op)) {
    case FuKind::ALU:
      for (FuUnit &u : alus)
        if (u.canAccept()) {
          unit = &u;
          break;
        }
      break;
    case FuKind::BRANCH:
      unit = brUnit.canAccept() ? &brUnit : nullptr;
      break;
    case FuKind::MUL:
      unit = mulUnit.canAccept() ? &mulUnit : nullptr;
      break;
    case FuKind::DIV:
      unit = divUnit.canAccept() ? &divUnit : nullptr;
      break;
    case FuKind::LSU:
      unit = agu.canAccept() ? &agu : nullptr;
      break;
    default:
      break;
    }
    if (!unit)
      continue; // the unit is busy; try a younger ready op instead

    const uint32_t a = usesRs1(I.op) ? prf.val[q->ps1] : 0;
    const uint32_t b = usesRs2(I.op) ? prf.val[q->ps2] : 0;
    const ExecResult r = execute(I, q->pc, a, b);

    FuOp op;
    op.valid = true;
    op.seq = q->seq;
    op.robIdx = q->robIdx;
    op.lsqIdx = q->lsqIdx;
    op.ins = I;
    op.pc = q->pc;
    op.pdst = q->pdst;
    op.value = r.value;
    op.redirect = r.redirect;
    op.target = r.target;
    unit->accept(op);
    note(vIS, I);
    q->valid = false;
    issued++;
    stats.issuedOps++;
  }
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
    const Fetched f = fetchQ.front();
    const Instr I = decode(f.raw);

    if (fuKindOf(I.op) == FuKind::NONE) {
      // System ops wait for the machine to empty completely (all
      // older work committed AND the store buffer drained), then run
      // alone and take effect at commit
      if (w != 0 || !rob.empty() || !sb.empty()) {
        if (w == 0) {
          stats.dsSerialize++;
          if (trace)
            events.push_back("serialize wait");
        }
        return;
      }
      if (recTimerArmed) { // dispatch resumed: close the flush window
        stats.recoveryLossCycles += stats.cycles - recTimerStart;
        recTimerArmed = false;
      }
      fetchQ.pop_front();
      RobEntry &e = rob.at(rob.alloc());
      e.seq = seqCtr++;
      e.pc = f.pc;
      e.ins = I;
      e.done = true;
      e.isSystem = true;
      e.sysA0 = rmap.map[10]; // the map is architectural right now
      e.sysA7 = rmap.map[17];
      note(vDS, I);
      return;
    }

    if (rob.full()) {
      if (w == 0)
        stats.dsRobFull++;
      return;
    }
    const bool mem = isLoad(I.op) || isStore(I.op);
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
    const bool hasDest = writesRd(I.op) && I.rd != 0;
    if (hasDest && freeList.empty()) {
      if (w == 0)
        stats.dsNoPreg++; // cannot happen with physRegs = 32 + robSize
      return;
    }

    if (recTimerArmed) { // dispatch resumed: close the flush window
      stats.recoveryLossCycles += stats.cycles - recTimerStart;
      recTimerArmed = false;
    }
    fetchQ.pop_front();
    const uint64_t seq = seqCtr++;
    const uint32_t robIdx = rob.alloc();
    RobEntry &e = rob.at(robIdx);
    e.seq = seq;
    e.pc = f.pc;
    e.ins = I;
    // Every entry keeps its fetch-time history snapshot: a flush from
    // ANY point (branch mispredict or load replay) must be able to
    // rewind the speculative GHR to before the squashed branches
    e.ghrBefore = f.ghrBefore;

    // Sources rename through the CURRENT map, including updates made
    // by the older instruction dispatched this same cycle, and before
    // the destination displaces anything (rs may equal rd)
    const uint8_t ps1 = rmap.map[I.rs1];
    const uint8_t ps2 = rmap.map[I.rs2];
    if (hasDest) {
      e.pdst = freeList.pop();
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
      le.ins = I;
      le.isStore = isStore(I.op);
      le.pdst = e.pdst;
      if (le.isStore) {
        // Address and data resolve independently: capture the data
        // now if its producer already wrote back, else watch its preg
        le.dataPreg = ps2;
        le.dataReady = prf.ready[ps2];
        if (le.dataReady)
          le.data = prf.val[ps2];
      }
    }
    if (fuKindOf(I.op) == FuKind::BRANCH) {
      e.isBranch = true;
      e.predictedTaken = f.predTaken;
      e.predictedTarget = f.predTarget;
      e.predIdx = f.predIdx;
    }

    IqEntry *q = iq.allocate();
    q->seq = seq;
    q->robIdx = robIdx;
    q->lsqIdx = lsqIdx;
    q->pc = f.pc;
    q->ins = I;
    q->pdst = e.pdst;
    q->ps1 = ps1;
    q->ps2 = ps2;
    q->ready1 = !usesRs1(I.op) || prf.ready[ps1];
    // Stores don't wait for their data to issue; only the address
    // operand gates them, and the LSQ captures the data when it
    // appears
    q->ready2 = !usesRs2(I.op) || isStore(I.op) || prf.ready[ps2];
    note(vDS, I);
  }
}

// Fetch: an aligned block of width instructions (minimum an 8-byte
// pair) per cycle, steered by the predictor. pc advances when the
// block arrives (the predictor may redirect it mid-block); a squash
// marks the in-flight fetch stale
void OoOCore::fetchStage() {
  if (fOutstanding && imem->hasResponse()) {
    MemResponse r = imem->response();
    fOutstanding = false;
    if (fStale)
      fStale = false;
    else
      processFetch(r);
  }
  if (!fOutstanding && !pendRedirect &&
      fetchQ.size() + fBytes / 4 <= (size_t)cfg.fetchQSize) {
    if (pc % 4 != 0) {
      fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
      exit(1);
    }
    if (imem->canAccept()) {
      MemRequest req;
      req.addr = pc & ~(fBytes - 1);
      req.size = fBytes;
      imem->access(req);
      fOutstanding = true;
      fBlock = req.addr;
      fFetchPc = pc;
      if (imem->hasResponse()) { // 1-cycle port: complete this cycle
        MemResponse r = imem->response();
        fOutstanding = false;
        processFetch(r);
      } else if (trace) {
        events.push_back("ifetch wait");
      }
    }
  }
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
    fetchQ.push_back(
        Fetched{wpc, raw, p.taken, p.target, p.phtIdx, p.ghrBefore});
    if (p.taken) {
      pc = p.target;
      return;
    }
  }
  pc = fBlock + fBytes;
}

void OoOCore::tickUnits() {
  for (FuUnit &u : alus)
    u.tick();
  brUnit.tick();
  mulUnit.tick();
  divUnit.tick();
  agu.tick();
}

// Minimal syscall interface (a7 = number, a0 = argument), acting on
// the architectural values captured at dispatch; the machine was
// drained, so they are final
void OoOCore::doSyscall(const RobEntry &e) {
  const uint32_t num = prf.val[e.sysA7];
  const uint32_t arg = prf.val[e.sysA0];
  switch (num) {
  case 1:
    printf("%d\n", (int32_t)arg);
    break;
  case 2:
    putchar((int)(arg & 0xFF));
    break;
  case 3:
    for (uint32_t a = arg; msys.peek8(a) != 0; a++)
      putchar(msys.peek8(a));
    break;
  case 4: // print a0 as 8 hex digits (architectural-test signatures)
    printf("%08x\n", arg);
    break;
  case 10:
  case 93:
    halted = true;
    exitCode = (int)(arg & 0xFF);
    break;
  default:
    fprintf(stderr, "warning: unknown syscall a7=%u at pc=0x%08x\n", num,
            e.pc);
    break;
  }
}

// One clock cycle: stages in reverse pipeline order (each consumes
// what the stage behind it produced in an earlier cycle), redirect at
// the clock edge, units and memory ticking last
void OoOCore::stepCycle() {
  stats.cycles++;
  // start-of-cycle occupancy sampling (averages in the final report)
  stats.robOccSum += rob.count();
  stats.iqOccSum += iq.count();
  stats.lsqOccSum += lsq.count();
  stats.sbOccSum += sb.count();
  stats.pregsUsedSum += (cfg.physRegs - 32) - freeList.count();
  events.clear();
  vDS = vIS = vWB = vCT = "";
  if (trace)
    captureView();

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

  tickUnits();
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
  snprintf(b, sizeof b, "fq%zu rob%2u iq%2u lsq%2u sb%u", fetchQ.size(),
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
