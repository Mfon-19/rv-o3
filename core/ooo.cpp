// Out-of-order pipeline stages executed in reverse order each cycle
// (commit -> writeback -> AGU/LSQ -> issue -> dispatch -> fetch) so freed
// resources become available cleanly without same-cycle structural races.

#include "core/ooo.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>

#include "sim/syscall.h"

namespace {

// High bit distinguishes committed store-buffer writes from speculative LSQ
// loads when responses arrive back from L1D.
constexpr uint64_t kStoreBufferTagBit = 1ull << 63;

uint64_t makeStoreTag(uint32_t slot, uint32_t txnId) {
  return kStoreBufferTagBit | (uint64_t(slot) << 32) | txnId;
}

uint64_t makeLoadTag(uint32_t slot, uint16_t generation) {
  return (uint64_t(slot) << 16) | generation;
}

}

OoOCore::OoOCore(const SimConfig &config, MemorySystem &memSystem)
    : config(config),
      memSystem(memSystem),
      imem(memSystem.imem),
      dmem(memSystem.dmem),
      freeList(config.physRegs),
      physRegs(config.physRegs),
      rob(config.robSize),
      iq(config.iqSize),
      lsq(config.lsqSize),
      storeBuffer(config.sbSize),
      predictor(config),
      alus(config.aluCount, FuUnit("alu", 1, true)),
      branchUnit("br", 1, true),
      mulUnit("mul", config.mulLatency, config.mulPipelined),
      divUnit("div", config.divLatency, false),
      agus(config.aguCount, FuUnit("agu", 1, true)),
      loadOutputs(config.width > 2 ? config.width : 2),
      fetchBlockBytes(config.fetchBlockBytes()),
      fetchQueue(config.fetchQSize) {
  for (FuUnit &alu : alus)
    execUnits.push_back(&alu);

  for (FuUnit *unit : {&branchUnit, &mulUnit, &divUnit})
    execUnits.push_back(unit);

  wbReady.reserve(execUnits.size() + loadOutputs.size());
  renameMap.reset();

  // Physical registers 0..31 start mapped to architectural x0..x31.
  for (uint32_t physReg = 32; physReg < config.physRegs; physReg++)
    freeList.push((uint8_t)physReg);
}

int OoOCore::run() {
  while (!halted) {
    if (stats.cycles >= config.maxCycles) {
      fprintf(stderr,
              "stopping after %" PRIu64
              " cycles without an exit syscall (raise with -c)\n",
              stats.cycles);
      exitCode = 2;
      break;
    }

    stepCycle();
  }

  printStats();
  return exitCode;
}

void OoOCore::printStats() const {
  auto ratio = [](uint64_t num, uint64_t denom, double scale = 1.0) {
    return denom ? scale * (double)num / (double)denom : 0.0;
  };

  fprintf(stderr,
          "--- rvsim: %" PRIu64 " cycles, %" PRIu64
          " instructions retired, IPC = %.3f (CPI = %.3f)\n",
          stats.cycles, stats.retired, ratio(stats.retired, stats.cycles),
          ratio(stats.cycles, stats.retired));

  fprintf(stderr,
          "--- rvsim: %" PRIu64 " branches, %" PRIu64
          " mispredicted (%.1f%%, %.2f MPKI), %" PRIu64 " flushes, %" PRIu64
          " forwarded, %" PRIu64 " replays\n",
          stats.branches, stats.mispredicts,
          ratio(stats.mispredicts, stats.branches, 100.0),
          ratio(stats.mispredicts, stats.retired, 1000.0), stats.flushes,
          stats.loadsForwarded, stats.loadReplays);

  if (!config.flatMemory || config.flatLatency > 1)
    memSystem.printStats(stats.retired);

  fprintf(stderr, "--- rvsim: exit code %d\n", exitCode);
}

void OoOCore::dumpRegs() const {
  uint32_t archRegs[32];
  for (int i = 0; i < 32; i++)
    archRegs[i] = reg(i);
  dumpRegisters(archRegs, pc);
}

// Retires completed instructions strictly in program order; this is the only
// stage that exposes faults, runs syscalls, or pushes stores to the store buffer.
void OoOCore::commitStage() {
  uint32_t committed = 0;

  while (committed < config.width && !rob.empty() && rob.head().done && !halted) {
    RobEntry &robEntry = rob.head();
    const Instr &instr = robEntry.instr;

    if (robEntry.isBranch && robEntry.actualTaken && (robEntry.actualTarget & 3)) {
      fprintf(stderr, "fatal: misaligned branch target 0x%08x at pc=0x%08x\n",
              robEntry.actualTarget, robEntry.pc);
      exit(1);
    }

    if (robEntry.fault) {
      const char *operation = isStore(instr.op) ? "store" : "load";
      if (robEntry.fault == 1)
        Memory::failMisaligned(lsq.head().addr, operation, robEntry.pc);
      memSystem.backing.failOutOfRange(lsq.head().addr, operation);
    }

    CommitRecord record;
    record.sequence = stats.retired;
    record.pc = robEntry.pc;
    record.instruction = instr.raw;

    if (robEntry.isSystem) {
      switch (instr.op) {
      case Op::ECALL: {
        stats.retired++;
        uint32_t &syscallNum = physRegs.values[robEntry.syscallNumPhysReg];
        uint32_t &syscallArg = physRegs.values[robEntry.syscallArgPhysReg];

        if (runSyscall(
                syscallNum, syscallArg, robEntry.pc, false,
                [&](uint32_t addr) { return memSystem.peek8(addr); }, exitCode,
                memSystem.backing.bytes.size())) {
          halted = true;
        }

        if (syscallReturnsInput(syscallNum))
          record.registerWrite = RegisterWrite{10, syscallArg};
        break;
      }

      case Op::EBREAK:
        stats.retired++;
        fprintf(stderr, "ebreak at pc=0x%08x; halting\n", robEntry.pc);
        halted = true;
        break;

      case Op::ILLEGAL:
        fprintf(stderr, "illegal instruction 0x%08x at pc=0x%08x\n", instr.raw,
                robEntry.pc);
        halted = true;
        exitCode = 1;
        record.exception = Exception{ExceptionKind::IllegalInstruction};
        break;

      default:
        stats.retired++;
        break;
      }
    } else {
      if (robEntry.isMem) {
        LsqEntry &lsqEntry = lsq.head();
        if (lsqEntry.isStore) {
          if (storeBuffer.full())
            break;

          storeBuffer.push(
              StoreBufferEntry{lsqEntry.addr, lsqEntry.size, lsqEntry.data});
          record.memoryWrite =
              MemoryWrite{lsqEntry.addr, lsqEntry.data, lsqEntry.size};
        }
        lsq.popHead();
      }

      if (robEntry.destPhysReg != kNoReg) {
        record.registerWrite =
            RegisterWrite{instr.rd, physRegs.values[robEntry.destPhysReg]};
        freeList.push(robEntry.prevPhysReg);
      }

      if (robEntry.isBranch) {
        stats.branches++;
        if (robEntry.mispredicted())
          stats.mispredicts++;

        if (config.usePredictor) {
          predictor.update(robEntry.pc, instr, robEntry.actualTaken,
                           robEntry.actualTarget, robEntry.phtIndex,
                           robEntry.predCheckpoint);
        }
      }

      stats.retired++;
    }

    if (onCommit)
      onCommit(record);

    rob.popHead();
    committed++;
  }
}

// Grants up to `wbPorts` writeback slots oldest-first, waking IQ waiters and
// triggering recovery immediately on branch misprediction.
void OoOCore::writebackStage() {
  const size_t grantCount = std::min((size_t)config.wbPorts, wbReady.size());

  for (size_t i = 0; i < grantCount; i++) {
    const auto oldestIt = std::min_element(
        wbReady.begin(), wbReady.end(),
        [](const FuOp *lhs, const FuOp *rhs) { return lhs->seq < rhs->seq; });

    FuOp &op = **oldestIt;
    *oldestIt = wbReady.back();
    wbReady.pop_back();

    RobEntry &robEntry = rob.at(op.robIdx);

    if (op.destPhysReg != kNoReg) {
      physRegs.values[op.destPhysReg] = op.value;
      physRegs.ready[op.destPhysReg] = true;
      iq.wakeup(op.destPhysReg);
    }

    robEntry.done = true;

    if (robEntry.isBranch) {
      robEntry.actualTaken = op.redirect;
      robEntry.actualTarget = op.redirect ? op.target : robEntry.pc + 4;

      if (robEntry.mispredicted()) {
        const uint64_t branchSeq = op.seq;
        const uint32_t targetPc = robEntry.actualTarget;

        if (config.usePredictor) {
          predictor.restore(robEntry.predCheckpoint);
          predictor.replay(robEntry.pc, robEntry.instr, robEntry.actualTaken);
        }

        op.valid = false;
        recover(branchSeq);
        pendingRedirect = true;
        pendingTarget = targetPc;
        break;
      }
    }

    op.valid = false;
  }
}

// Flushes every speculative instruction younger than `keepSeq` and rolls back
// the rename map by walking the ROB from tail to `keepSeq`.
void OoOCore::recover(uint64_t keepSeq) {
  stats.flushes++;
  iq.flushYounger(keepSeq);

  for (FuUnit *unit : execUnits)
    unit->flushYounger(keepSeq);
  for (FuUnit &agu : agus)
    agu.flushYounger(keepSeq);
  for (FuOp &loadOut : loadOutputs) {
    if (loadOut.valid && loadOut.seq > keepSeq)
      loadOut.valid = false;
  }

  wbReady.erase(
      std::remove_if(wbReady.begin(), wbReady.end(),
                     [](const FuOp *op) { return !op->valid; }),
      wbReady.end());

  while (lsq.count() && lsq.tail().seq > keepSeq)
    lsq.popTail();

  while (rob.count() && rob.tail().seq > keepSeq) {
    RobEntry &tailEntry = rob.tail();
    if (tailEntry.destPhysReg != kNoReg) {
      renameMap.archToPhys[tailEntry.instr.rd] = tailEntry.prevPhysReg;
      freeList.push(tailEntry.destPhysReg);
    }
    rob.popTail();
  }

  fetchQueue.clear();
  if (fetchOutstanding)
    fetchStale = true;
}

// Consumes completed AGU effective addresses oldest-first, recording faults
// for commit-time delivery and scanning for speculative load violations.
void OoOCore::aguDrain() {
  for (;;) {
    FuUnit *oldestReady = nullptr;
    for (FuUnit &agu : agus) {
      if (agu.output.valid &&
          (!oldestReady || agu.output.seq < oldestReady->output.seq)) {
        oldestReady = &agu;
      }
    }

    if (!oldestReady)
      return;

    const FuOp op = oldestReady->output;
    oldestReady->output.valid = false;

    LsqEntry &lsqEntry = lsq.at(op.lsqIdx);
    lsqEntry.addr = op.value;
    lsqEntry.size = (uint8_t)accessSize(lsqEntry.op);
    lsqEntry.addrValid = true;

    RobEntry &robEntry = rob.at(op.robIdx);
    if (lsqEntry.addr % lsqEntry.size != 0)
      robEntry.fault = 1;
    else if ((uint64_t)lsqEntry.addr + lsqEntry.size > memSystem.backing.bytes.size())
      robEntry.fault = 2;

    if (lsqEntry.isStore) {
      if (lsqEntry.dataReady)
        robEntry.done = true;

      if (config.memOrder == MemOrder::Speculative && !robEntry.fault)
        violationScan(lsqEntry);
    }
  }
}

// Detects if any younger load already read memory overlapping this newly
// resolved store; if so, replays starting at the oldest offending load.
void OoOCore::violationScan(const LsqEntry &store) {
  const LsqEntry *victim = nullptr;

  for (uint32_t i = 0; i < lsq.count(); i++) {
    const LsqEntry &entry = lsq.nth(i);
    if (!entry.isStore && (entry.issued || entry.done) &&
        entry.seq > store.seq &&
        overlaps(store.addr, store.size, entry.addr, entry.size)) {
      victim = &entry;
      break;
    }
  }

  if (!victim)
    return;

  stats.loadReplays++;

  const uint64_t victimSeq = victim->seq;
  const uint32_t victimPc = victim->pc;

  if (config.usePredictor)
    predictor.restore(rob.at(victim->robIdx).predCheckpoint);

  recover(victimSeq - 1);
  pendingRedirect = true;
  pendingTarget = victimPc;
}

void OoOCore::drainDataResponses() {
  while (dmem->hasResponse()) {
    const MemResponse &response = dmem->frontResponse();
    const uint64_t tag = response.tag;
    const uint32_t data = response.readData;
    dmem->popResponse();

    if (tag & kStoreBufferTagBit) {
      const uint32_t txnId = (uint32_t)tag;
      const uint32_t slot = uint32_t((tag & ~kStoreBufferTagBit) >> 32);

      if (storeBuffer.live(slot)) {
        StoreBufferEntry &storeEntry = storeBuffer.at(slot);
        if (storeEntry.inFlight && !storeEntry.acked && storeEntry.txnId == txnId)
          storeEntry.acked = true;
      }
      continue;
    }

    const uint32_t lsqSlot = (uint32_t)(tag >> 16);
    const uint16_t generation = (uint16_t)tag;

    // Ignore responses belonging to loads that were already squashed/reused.
    if (!lsq.live(lsqSlot))
      continue;

    LsqEntry &lsqEntry = lsq.at(lsqSlot);
    if (lsqEntry.isStore || !lsqEntry.issued || lsqEntry.done ||
        lsqEntry.generation != generation) {
      continue;
    }

    lsqEntry.value = extendLoad(lsqEntry.op, data);
    lsqEntry.done = true;
  }
}

// Captures ready store data, reports completed loads to writeback, checks
// store-to-load forwarding, and issues up to `dataPorts` accesses to L1D.
void OoOCore::lsqOperate() {
  drainDataResponses();

  while (!storeBuffer.empty() && storeBuffer.head().acked)
    storeBuffer.popHead();

  size_t nextOutput = 0;
  for (uint32_t i = 0; i < lsq.count(); i++) {
    LsqEntry &entry = lsq.nth(i);

    if (entry.isStore && !entry.dataReady && physRegs.ready[entry.dataPhysReg]) {
      entry.data = physRegs.values[entry.dataPhysReg];
      entry.dataReady = true;
      if (entry.addrValid)
        rob.at(entry.robIdx).done = true;
    }

    if (!entry.isStore && entry.done && !entry.reported) {
      while (nextOutput < loadOutputs.size() && loadOutputs[nextOutput].valid)
        nextOutput++;

      if (nextOutput < loadOutputs.size()) {
        FuOp &slot = loadOutputs[nextOutput++];
        slot.valid = true;
        slot.seq = entry.seq;
        slot.robIdx = entry.robIdx;
        slot.destPhysReg = entry.destPhysReg;
        slot.value = entry.value;
        wbReady.push_back(&slot);
        entry.reported = true;
      }
    }
  }

  for (uint32_t port = 0; port < config.dataPorts; ++port) {
    LsqEntry *selectedLoad = nullptr;
    uint32_t selectedSlot = 0;

    const uint64_t oldestUnknownSeq = lsq.oldestUnknownStore();
    const uint64_t oldestPendingSeq =
        config.memOrder == MemOrder::Conservative ? lsq.oldestPendingLoad()
                                                  : UINT64_MAX;

    for (uint32_t i = 0; i < lsq.count(); i++) {
      LsqEntry &loadEntry = lsq.nth(i);
      if (loadEntry.isStore || !loadEntry.addrValid || loadEntry.issued || loadEntry.done)
        continue;

      if (loadEntry.seq > oldestPendingSeq)
        break;

      // Faulting loads skip memory and complete immediately so commit raises the fault.
      if (rob.at(loadEntry.robIdx).fault) {
        loadEntry.value = 0;
        loadEntry.done = true;
        continue;
      }

      if (oldestUnknownSeq < loadEntry.seq && config.memOrder != MemOrder::Speculative)
        continue;

      bool blocked = false;
      bool forwarded = false;

      auto checkOlderStore = [&](uint32_t storeAddr, uint32_t storeSize,
                                 bool storeDataReady, uint32_t storeData) {
        if (storeAddr == loadEntry.addr && storeSize == loadEntry.size) {
          if (!storeDataReady) {
            blocked = true;
          } else {
            loadEntry.value = extendLoad(loadEntry.op, storeData);
            loadEntry.done = true;
            forwarded = true;
          }
        } else if (overlaps(storeAddr, storeSize, loadEntry.addr, loadEntry.size)) {
          blocked = true;
        }
      };

      // Search youngest-to-oldest older stores in the LSQ, then the store buffer.
      for (int j = (int)i - 1; j >= 0 && !blocked && !forwarded; j--) {
        const LsqEntry &storeEntry = lsq.nth((uint32_t)j);
        if (storeEntry.isStore && storeEntry.addrValid)
          checkOlderStore(storeEntry.addr, storeEntry.size, storeEntry.dataReady, storeEntry.data);
      }

      for (int j = (int)storeBuffer.count() - 1; j >= 0 && !blocked && !forwarded; j--) {
        const StoreBufferEntry &storeEntry = storeBuffer.nth((uint32_t)j);
        checkOlderStore(storeEntry.addr, storeEntry.size, true, storeEntry.data);
      }

      if (forwarded) {
        stats.loadsForwarded++;
        if (config.memOrder == MemOrder::Conservative)
          break;
        continue;
      }

      if (!blocked) {
        selectedLoad = &loadEntry;
        selectedSlot = lsq.indexOf(i);
        break;
      }

      if (config.memOrder == MemOrder::Conservative)
        break;
    }

    bool sentAccess = false;
    StoreBufferEntry *storeEntry = storeBuffer.nextToIssue();

    // Prioritize draining the store buffer when full or when no load is ready.
    if (storeEntry && (storeBuffer.full() || !selectedLoad)) {
      if (dmem->canAccept()) {
        MemRequest req;
        req.addr = storeEntry->addr;
        req.size = storeEntry->size;
        req.isWrite = true;
        req.writeData = storeEntry->data;
        req.tag = makeStoreTag(storeBuffer.nextIssueSlot(), storeBufferTxnCounter);

        storeEntry->txnId = storeBufferTxnCounter++;
        storeBuffer.issue();
        dmem->access(req);
        sentAccess = true;
      }
    } else if (selectedLoad && dmem->canAccept()) {
      MemRequest req;
      req.addr = selectedLoad->addr;
      req.size = selectedLoad->size;
      req.tag = makeLoadTag(selectedSlot, selectedLoad->generation);

      selectedLoad->issued = true;
      dmem->access(req);
      sentAccess = true;
    }

    if (!sentAccess)
      break;
  }

  drainDataResponses();
}

FuUnit *OoOCore::acquireUnit(FuKind kind, size_t &nextAlu, size_t &nextAgu) {
  switch (kind) {
  case FuKind::ALU:
    while (nextAlu < alus.size() && !alus[nextAlu].canAccept())
      nextAlu++;
    return nextAlu < alus.size() ? &alus[nextAlu++] : nullptr;

  case FuKind::BRANCH: return &branchUnit;
  case FuKind::MUL:    return &mulUnit;
  case FuKind::DIV:    return &divUnit;

  case FuKind::LSU:
    while (nextAgu < agus.size() && !agus[nextAgu].canAccept())
      nextAgu++;
    return nextAgu < agus.size() ? &agus[nextAgu++] : nullptr;

  case FuKind::NONE:
    return nullptr;
  }

  return nullptr;
}

// Issues up to `width` ready instructions oldest-first to available functional units.
void OoOCore::issueStage() {
  uint32_t issued = 0;
  size_t nextAlu = 0;
  size_t nextAgu = 0;
  size_t kept = 0;

  for (size_t i = 0; i < iq.entries.size(); i++) {
    IqEntry &entry = iq.entries[i];

    if (issued < config.width && entry.ready1 && entry.ready2) {
      FuUnit *targetUnit = acquireUnit(entry.unit, nextAlu, nextAgu);
      if (targetUnit && targetUnit->canAccept()) {
        const RobEntry &robEntry = rob.at(entry.robIdx);
        const ExecResult execResult = execute(
            robEntry.instr, robEntry.pc, physRegs.values[entry.src1PhysReg],
            physRegs.values[entry.src2PhysReg]);

        FuOp op;
        op.valid = true;
        op.seq = entry.seq;
        op.robIdx = entry.robIdx;
        op.lsqIdx = entry.lsqIdx;
        op.destPhysReg = robEntry.destPhysReg;
        op.value = execResult.value;
        op.redirect = execResult.redirect;
        op.target = execResult.target;

        targetUnit->accept(op);
        issued++;
        continue;
      }
    }

    if (kept != i)
      iq.entries[kept] = entry;
    kept++;
  }

  iq.entries.resize(kept);
}

// Decodes, renames, and allocates ROB, IQ, and LSQ entries up to `width` per cycle.
void OoOCore::dispatchStage() {
  for (uint32_t lane = 0; lane < config.width; lane++) {
    if (fetchQueue.empty())
      return;

    const FetchedInstr fetched = fetchQueue.head();
    const Instr instr = decode(fetched.raw);
    const FuKind unitKind = fuKindOf(instr.op);
    const bool isSystemOp = unitKind == FuKind::NONE;
    const bool isMemOp = isLoad(instr.op) || isStore(instr.op);
    const bool hasDest = writesRd(instr.op) && instr.rd != 0;

    // System ops (ECALL, EBREAK, FENCE, ILLEGAL) serialize on an empty machine.
    if (isSystemOp && (lane != 0 || !rob.empty() || !storeBuffer.empty()))
      return;
    if (rob.full() || (isMemOp && lsq.full()) || iq.full() || (hasDest && freeList.empty()))
      return;

    fetchQueue.popHead();

    const uint64_t seq = nextSeq++;
    const uint32_t robIdx = rob.alloc();
    RobEntry &robEntry = rob.at(robIdx);
    robEntry.seq = seq;
    robEntry.pc = fetched.pc;
    robEntry.instr = instr;
    robEntry.predCheckpoint = fetched.predCheckpoint;

    if (isSystemOp) {
      robEntry.done = true;
      robEntry.isSystem = true;
      robEntry.syscallArgPhysReg = renameMap.archToPhys[10];
      robEntry.syscallNumPhysReg = renameMap.archToPhys[17];
      return;
    }

    const uint8_t src1PhysReg = renameMap.archToPhys[instr.rs1];
    const uint8_t src2PhysReg = renameMap.archToPhys[instr.rs2];

    if (hasDest) {
      robEntry.destPhysReg = freeList.head();
      freeList.popHead();
      robEntry.prevPhysReg = renameMap.archToPhys[instr.rd];
      renameMap.archToPhys[instr.rd] = robEntry.destPhysReg;
      physRegs.ready[robEntry.destPhysReg] = false;
    }

    robEntry.isMem = isMemOp;
    uint32_t lsqIdx = 0;

    if (isMemOp) {
      lsqIdx = lsq.alloc();
      LsqEntry &lsqEntry = lsq.at(lsqIdx);
      lsqEntry.seq = seq;
      lsqEntry.robIdx = robIdx;
      lsqEntry.pc = fetched.pc;
      lsqEntry.op = instr.op;
      lsqEntry.isStore = isStore(instr.op);
      lsqEntry.destPhysReg = robEntry.destPhysReg;

      if (lsqEntry.isStore) {
        lsqEntry.dataPhysReg = src2PhysReg;
        lsqEntry.dataReady = physRegs.ready[src2PhysReg];
        if (lsqEntry.dataReady)
          lsqEntry.data = physRegs.values[src2PhysReg];
      }
    }

    if (unitKind == FuKind::BRANCH) {
      robEntry.isBranch = true;
      robEntry.predictedTaken = fetched.predTaken;
      robEntry.predictedTarget = fetched.predTarget;
      robEntry.phtIndex = fetched.phtIndex;
    }

    // Store data (rs2) is captured directly in the LSQ, so the AGU only waits on rs1.
    IqEntry iqEntry;
    iqEntry.seq = seq;
    iqEntry.robIdx = robIdx;
    iqEntry.lsqIdx = lsqIdx;
    iqEntry.src1PhysReg = src1PhysReg;
    iqEntry.src2PhysReg = src2PhysReg;
    iqEntry.ready1 = !usesRs1(instr.op) || physRegs.ready[src1PhysReg];
    iqEntry.ready2 = !usesRs2(instr.op) || isStore(instr.op) || physRegs.ready[src2PhysReg];
    iqEntry.unit = unitKind;
    iq.push(iqEntry);
  }
}

void OoOCore::fetchStage() {
  auto consumeResponse = [&] {
    const MemResponse &response = imem->frontResponse();
    fetchOutstanding = false;

    if (fetchStale)
      fetchStale = false;
    else
      processFetch(response);

    imem->popResponse();
  };

  if (fetchOutstanding && imem->hasResponse())
    consumeResponse();

  if (fetchOutstanding || pendingRedirect ||
      fetchQueue.count() + fetchBlockBytes / 4 > config.fetchQSize) {
    return;
  }

  // Wrong-path speculative PCs may be misaligned or out of range; stall fetch
  // until the older branch redirects or the machine drains and faults precisely.
  if (pc % 4 != 0)
    return;

  if (pc >= memSystem.backing.bytes.size()) {
    if (rob.empty() && fetchQueue.count() == 0)
      memSystem.backing.failOutOfRange(pc, "instruction fetch");
    return;
  }

  if (!imem->canAccept())
    return;

  MemRequest req;
  req.addr = pc & ~(fetchBlockBytes - 1);
  req.size = fetchBlockBytes;

  imem->access(req);
  fetchOutstanding = true;
  fetchBlockAddr = req.addr;
  fetchStartPc = pc;

  if (imem->hasResponse())
    consumeResponse();
}

void OoOCore::processFetch(const MemResponse &response) {
  const uint32_t startWord = (fetchStartPc - fetchBlockAddr) / 4;
  const uint32_t blockWords = fetchBlockBytes / 4;

  for (uint32_t wordIdx = startWord; wordIdx < blockWords; wordIdx++) {
    const uint32_t rawWord = readLittleEndian(response.readLine.data() + wordIdx * 4, 4);
    const uint32_t instrPc = fetchBlockAddr + wordIdx * 4;

    Predictor::Prediction prediction;
    if (config.usePredictor)
      prediction = predictor.predict(instrPc);

    fetchQueue.push(FetchedInstr{instrPc, rawWord, prediction.taken,
                                 prediction.target, prediction.phtIndex,
                                 prediction.before});

    // Stop consuming the fetch block on the first predicted-taken control transfer.
    if (prediction.taken) {
      pc = prediction.target;
      return;
    }
  }

  pc = fetchBlockAddr + fetchBlockBytes;
}

void OoOCore::stepCycle() {
  stats.cycles++;

  commitStage();
  if (halted)
    return;

  writebackStage();
  aguDrain();
  lsqOperate();
  issueStage();
  dispatchStage();
  fetchStage();

  if (pendingRedirect) {
    if (fetchOutstanding)
      fetchStale = true;
    pc = pendingTarget;
    pendingRedirect = false;
  }

  for (FuUnit *unit : execUnits) {
    if (unit->tick())
      wbReady.push_back(&unit->output);
  }

  for (FuUnit &agu : agus)
    agu.tick();

  memSystem.tick();

  if (config.trace) {
    fprintf(stderr, "cyc %6" PRIu64 " | pc=0x%08x fq=%u rob=%u iq=%u lsq=%u sb=%u\n",
            stats.cycles, pc, fetchQueue.count(), rob.count(), iq.count(),
            lsq.count(), storeBuffer.count());
  }
}
