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
      trace(cfg.trace) {
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
          " mispredicted (%.1f%%), %" PRIu64 " flushes; avg issue %.2f"
          "; wb-port holds %" PRIu64 "\n",
          stats.branches, stats.mispredicts,
          stats.branches ? 100.0 * (double)stats.mispredicts /
                               (double)stats.branches
                         : 0.0,
          stats.flushes,
          stats.cycles ? (double)stats.issuedOps / (double)stats.cycles : 0.0,
          stats.wbConflicts);
  fprintf(stderr, "--- rvsim: %" PRIu64 " loads forwarded from stores, %"
          PRIu64 " store-buffer commit stalls\n",
          stats.loadsForwarded, stats.sbCommitStalls);
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
// program order — the only place architectural state changes
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
        fprintf(stderr, "ebreak at pc=0x%08x — halting\n", e.pc);
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
    stats.retired++;
    note(vCT, I);
    if (onCommit)
      onCommit(rec);
    rob.popHead();
    done++;
  }
}

// Writeback: oldest-first over the units' output slots plus the load
// completion slot, up to wbPorts per cycle. Winners write the physical
// register file, wake the issue queue, and mark their ROB entry done.
// Branches resolve here — a mispredict triggers recovery and stops the
// (younger) rest of the cycle's writebacks, whose slots are flushed
void OoOCore::writebackStage() {
  std::vector<FuOp *> cands;
  for (FuUnit &u : alus)
    if (u.out.valid)
      cands.push_back(&u.out);
  for (FuUnit *u : {&brUnit, &mulUnit, &divUnit})
    if (u->out.valid)
      cands.push_back(&u->out);
  if (loadOut.valid)
    cands.push_back(&loadOut);
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
// in-flight load, LSQ tail, and the ROB tail — walking youngest-first
// so each entry restores the mapping it displaced and returns its
// physical register. The frontend queue empties too; the pc itself is
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
  if (loadOut.valid && loadOut.seq > seq)
    loadOut = FuOp{};
  if (dport == DPort::LOAD && dLoadSeq > seq)
    dport = DPort::LOAD_STALE; // drain and drop the response
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
}

// The AGU's output feeds the LSQ, not a writeback port: fill in the
// address (and store data), record any fault for commit to act on,
// and mark stores complete — a resolved store has done all it will
// do until commit releases it
void OoOCore::aguDrain() {
  if (!agu.out.valid)
    return;
  FuOp op = agu.out;
  agu.out = FuOp{};
  LsqEntry &le = lsq.at(op.lsqIdx);
  le.addr = op.value;
  le.size = (uint8_t)accessSize(op.ins.op);
  le.addrValid = true;
  if (le.isStore)
    le.data = op.storeData;

  RobEntry &e = rob.at(op.robIdx);
  if (le.addr % le.size != 0)
    e.fault = 1;
  else if ((uint64_t)le.addr + le.size > msys.backing.bytes.size())
    e.fault = 2;
  if (le.isStore)
    e.done = true;
}

// The conservative memory engine: finish the port's current customer,
// surface one completed load per cycle to the writeback arbiter, then
// choose the port's next customer — the oldest eligible load, or the
// store buffer (which has priority when full, since it backpressures
// commit)
void OoOCore::lsqOperate() {
  // A squashed load's response must still be drained from the port
  if (dport == DPort::LOAD_STALE && dmem->done()) {
    dmem->response();
    dport = DPort::FREE;
  }
  if (dport == DPort::LOAD) {
    if (dmem->done()) {
      MemResponse r = dmem->response();
      dport = DPort::FREE;
      LsqEntry &le = lsq.at(dLoadLsqIdx);
      le.value = extendLoad(le.ins.op, r.rdata);
      le.done = true;
    } else {
      stats.dataStallCycles++;
      if (trace)
        events.push_back("dmem wait");
    }
  }
  if (dport == DPort::STORE && dmem->done()) {
    dmem->response();
    dport = DPort::FREE;
    sb.popHead();
  }

  // Surface a completed load to the writeback arbiter (its slot also
  // backpressures: an arbitration loss holds everything here)
  if (!loadOut.valid) {
    LsqEntry *best = nullptr;
    for (uint32_t k = 0; k < lsq.count(); k++) {
      LsqEntry &le = lsq.nth(k);
      if (!le.isStore && le.done && !le.reported &&
          (!best || le.seq < best->seq))
        best = &le;
    }
    if (best) {
      loadOut.valid = true;
      loadOut.seq = best->seq;
      loadOut.robIdx = best->robIdx;
      loadOut.ins = best->ins;
      loadOut.pc = best->pc;
      loadOut.pdst = best->pdst;
      loadOut.value = best->value;
      best->reported = true;
    }
  }

  if (dport != DPort::FREE)
    return;

  // Find the oldest load allowed to execute. Walking oldest-first, an
  // unresolved store address is a wall: nothing younger may pass it.
  // A blocked load is a wall too — loads execute in order among
  // themselves (conservative)
  LsqEntry *cand = nullptr;
  uint32_t candRing = 0;
  for (uint32_t k = 0; k < lsq.count(); k++) {
    LsqEntry &le = lsq.nth(k);
    if (le.isStore) {
      if (!le.addrValid)
        break;
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
    // Scan older stores newest-first: LSQ entries before k, then the
    // store buffer (all committed, hence older)
    bool blocked = false, forwarded = false;
    for (int j = (int)k - 1; j >= 0 && !blocked && !forwarded; j--) {
      LsqEntry &st = lsq.nth((uint32_t)j);
      if (!st.isStore)
        continue;
      if (st.addr == le.addr && st.size == le.size) {
        le.value = extendLoad(le.ins.op, st.data);
        le.done = true;
        forwarded = true;
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
      if (trace)
        events.push_back("store->load forward");
    } else if (!blocked) {
      cand = &le;
      candRing = lsq.indexOf(k);
    }
    break; // in-order loads: only the oldest unexecuted one is eligible
  }

  const bool sbWants = !sb.empty();
  if (sbWants && (sb.full() || !cand)) {
    StoreBufEntry &st = sb.head();
    MemRequest req;
    req.addr = st.addr;
    req.size = st.size;
    req.isWrite = true;
    req.wdata = st.data;
    dmem->access(req);
    dport = DPort::STORE;
    if (dmem->done()) {
      dmem->response();
      dport = DPort::FREE;
      sb.popHead();
    }
  } else if (cand) {
    MemRequest req;
    req.addr = cand->addr;
    req.size = cand->size;
    dmem->access(req);
    dport = DPort::LOAD;
    dLoadLsqIdx = candRing;
    dLoadSeq = cand->seq;
    if (dmem->done()) { // combinational hit
      MemResponse r = dmem->response();
      dport = DPort::FREE;
      cand->value = extendLoad(cand->ins.op, r.rdata);
      cand->done = true;
    }
  }
}

// Issue: oldest-ready-first from the issue queue, up to width per
// cycle, gated on functional-unit availability. Operand values come
// from the physical register file — writeback ran earlier this cycle,
// so a ready bit set today is readable today
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
      continue; // structural: try a younger ready op

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
    op.storeData = b;
    unit->accept(op);
    note(vIS, I);
    q->valid = false;
    issued++;
    stats.issuedOps++;
  }
}

// Dispatch: up to width per cycle, in program order — decode, rename,
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
      // System ops serialize: alone, into an empty machine (all older
      // work committed AND drained), taking effect at commit
      if (w != 0 || !rob.empty() || !sb.empty()) {
        if (w == 0) {
          stats.dsSerialize++;
          if (trace)
            events.push_back("serialize wait");
        }
        return;
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

    fetchQ.pop_front();
    const uint64_t seq = seqCtr++;
    const uint32_t robIdx = rob.alloc();
    RobEntry &e = rob.at(robIdx);
    e.seq = seq;
    e.pc = f.pc;
    e.ins = I;

    // Sources rename through the CURRENT map — including updates made
    // by the older instruction dispatched this same cycle — and before
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
    }
    if (fuKindOf(I.op) == FuKind::BRANCH) {
      e.isBranch = true;
      e.predictedTaken = f.predTaken;
      e.predictedTarget = f.predTarget;
      e.predIdx = f.predIdx;
      e.ghrBefore = f.ghrBefore;
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
    q->ready2 = !usesRs2(I.op) || prf.ready[ps2];
    note(vDS, I);
  }
}

// Fetch: an aligned 8-byte pair per cycle, steered by the predictor.
// pc advances when the pair arrives (the predictor may redirect it
// mid-pair); a squash marks the in-flight fetch stale
void OoOCore::fetchStage() {
  if (fOutstanding && imem->done()) {
    MemResponse r = imem->response();
    fOutstanding = false;
    if (fStale)
      fStale = false;
    else
      processFetch(r);
  }
  if (!fOutstanding && !pendRedirect &&
      fetchQ.size() + 2 <= (size_t)cfg.fetchQSize) {
    if (pc % 4 != 0) {
      fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
      exit(1);
    }
    if (imem->canAccept()) {
      MemRequest req;
      req.addr = pc & ~7u;
      req.size = 8;
      imem->access(req);
      fOutstanding = true;
      fBlock = pc & ~7u;
      fFetchPc = pc;
      if (imem->done()) { // 1-cycle port: complete within this cycle
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
  uint32_t w[2];
  for (int i = 0; i < 2; i++)
    w[i] = (uint32_t)r.rline[i * 4] | (uint32_t)r.rline[i * 4 + 1] << 8 |
           (uint32_t)r.rline[i * 4 + 2] << 16 |
           (uint32_t)r.rline[i * 4 + 3] << 24;
  for (uint32_t idx = (fFetchPc - fBlock) / 4; idx < 2; idx++) {
    const uint32_t wpc = fBlock + idx * 4;
    Predictor::Pred p;
    if (cfg.usePredictor)
      p = pred.predict(wpc);
    fetchQ.push_back(
        Fetched{wpc, w[idx], p.taken, p.target, p.phtIdx, p.ghrBefore});
    if (p.taken) {
      pc = p.target;
      return;
    }
  }
  pc = fBlock + 8;
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
// the architectural values captured at dispatch — the machine was
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
