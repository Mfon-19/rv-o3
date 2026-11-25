// Out-of-order pipeline stages executed in reverse order each cycle
// (commit -> writeback -> AGU/LSQ -> issue -> dispatch -> fetch) so freed
// resources become available cleanly without same-cycle structural races.

#include "core/ooo.h"

#include <algorithm>
#include <bit>
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
      l1iCache(memSystem.l1i ? &*memSystem.l1i : nullptr),
      l1dCache(memSystem.l1d ? &*memSystem.l1d : nullptr),
      flatFastPath(config.flatMemory && config.flatLatency == 1),
      freeList(config.physRegs),
      physRegs(config.physRegs),
      rob(config.robSize),
      iq(config.iqSize),
      lsq(config.lsqSize),
      storeBuffer(config.sbSize),
      predictor(config),
      alus(config.aluCount, FuUnit("alu", 1, true)),
      branchUnits(config.branchCount, FuUnit("br", 1, true)),
      mulUnit("mul", config.mulLatency, config.mulPipelined),
      divUnit("div", config.divLatency, false),
      loadOutputs(std::bit_ceil(config.lsqSize ? config.lsqSize : 1u)),
      lsqUseWaitMask(std::bit_ceil(config.lsqSize ? config.lsqSize : 1u) <= 64),
      fetchBlockBytes(config.fetchBlockBytes()),
      fetchQueue(config.fetchQSize) {
  for (FuUnit &alu : alus)
    execUnits.push_back(&alu);

  branchExecBegin = (uint32_t)execUnits.size();
  for (FuUnit &br : branchUnits)
    execUnits.push_back(&br);
  branchExecEnd = (uint32_t)execUnits.size();

  mulExecIdx = (uint32_t)execUnits.size();
  execUnits.push_back(&mulUnit);

  divExecIdx = (uint32_t)execUnits.size();
  execUnits.push_back(&divUnit);

  renameMap.reset();

  // Physical registers 0..31 start mapped to architectural x0..x31.
  for (uint32_t physReg = 32; physReg < config.physRegs; physReg++)
    freeList.push((uint8_t)physReg);
}

const OoOCore::DecodedEntry &OoOCore::decodeCached(uint32_t instrPc, uint32_t raw) {
  DecodedEntry &slot = decodeCache[(instrPc >> 2) & (kDecodeCacheSize - 1)];
  if (slot.instr.raw != raw || raw == 0) {
    const Instr instr = decode(raw);
    slot.instr = instr;
    slot.unit = fuKindOf(instr.op);
    slot.flags = (uint8_t)(((writesRd(instr.op) && instr.rd != 0) ? 1u : 0u) |
                           (usesRs1(instr.op) ? 2u : 0u) |
                           (usesRs2(instr.op) ? 4u : 0u) |
                           (isStore(instr.op) ? 8u : 0u));
    slot.memSize = (uint8_t)accessSize(instr.op);
  }
  return slot;
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
  const bool trackCommit = (bool)onCommit;

  while (committed < config.width && !rob.empty() && rob.head().done && !halted) {
    RobEntry &robEntry = rob.head();

    // Fast path for ordinary ALU/MUL/DIV instructions with no special commit actions.
    if (!trackCommit && !robEntry.isMem && !robEntry.isBranch &&
        !robEntry.isSystem && !robEntry.fault) {
      if (robEntry.destPhysReg != kNoReg)
        freeList.push(robEntry.prevPhysReg);
      stats.retired++;
      rob.popHead();
      committed++;
      continue;
    }

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
    if (trackCommit) {
      record.sequence = stats.retired;
      record.pc = robEntry.pc;
      record.instruction = instr.raw;
    }

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

        if (trackCommit && syscallReturnsInput(syscallNum))
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
        if (trackCommit)
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
          if (trackCommit)
            record.memoryWrite = MemoryWrite{lsqEntry.addr, lsqEntry.data, lsqEntry.size};
        } else {
          lsqSpecIssuedLoads--;
        }
        lsq.popHead();
      }

      if (robEntry.destPhysReg != kNoReg) {
        if (trackCommit) {
          record.registerWrite =
              RegisterWrite{instr.rd, physRegs.values[robEntry.destPhysReg]};
        }
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

    if (trackCommit)
      onCommit(record);

    rob.popHead();
    committed++;
  }
}

// Grants up to `wbPorts` writeback slots oldest-first, waking IQ waiters and
// triggering recovery immediately on branch misprediction.
void OoOCore::writebackStage() {
  auto wakeDependents = [&](uint8_t physReg, uint32_t value) {
    physRegs.values[physReg] = value;
    physRegs.ready[physReg] = true;
    if (iq.usesBitmask())
      iq.wakeupBitmask(physReg);
    else
      iq.wakeup(physReg);

    if (lsqUseWaitMask) {
      if (uint64_t waitMask = storeDataWaitMask[physReg]) {
        storeDataWaitMask[physReg] = 0;
        do {
          const uint32_t lsqSlot = (uint32_t)std::countr_zero(waitMask);
          waitMask &= waitMask - 1;
          LsqEntry &store = lsq.at(lsqSlot);
          store.data = value;
          store.dataReady = true;
          lsqStoresWaitingData--;
          if (store.addrValid)
            rob.at(store.robIdx).done = true;
        } while (waitMask);
      }
    }
  };

  // When every ready op fits in wbPorts and none is a branch, writeback order
  // cannot trigger recovery and all ops target distinct renamed registers.
  if (wbReadyCount <= config.wbPorts && wbBranchCount == 0) {
    const uint32_t count = wbReadyCount;
    wbReadyCount = 0;

    for (uint32_t i = 0; i < count; i++) {
      FuOp &op = *wbReady[i];
      if (op.destPhysReg != kNoReg)
        wakeDependents(op.destPhysReg, op.value);
      rob.at(op.robIdx).done = true;
      op.valid = false;
    }
    return;
  }

  const uint32_t grantCount = std::min(config.wbPorts, wbReadyCount);

  for (uint32_t i = 0; i < grantCount; i++) {
    uint32_t oldestIdx = 0;
    if (wbReadyCount > 1) {
      uint64_t oldestSeq = wbReady[0]->seq;
      for (uint32_t j = 1; j < wbReadyCount; j++) {
        if (wbReady[j]->seq < oldestSeq) {
          oldestSeq = wbReady[j]->seq;
          oldestIdx = j;
        }
      }
    }

    FuOp &op = *wbReady[oldestIdx];
    wbReady[oldestIdx] = wbReady[--wbReadyCount];

    RobEntry &robEntry = rob.at(op.robIdx);

    if (op.destPhysReg != kNoReg)
      wakeDependents(op.destPhysReg, op.value);

    robEntry.done = true;

    if (robEntry.isBranch) {
      wbBranchCount--;
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

  activeExecMask = 0;
  for (uint32_t i = 0; i < execUnits.size(); i++) {
    execUnits[i]->flushYounger(keepSeq);
    if (execUnits[i]->busy())
      activeExecMask |= (1ull << i);
  }

  uint32_t keptAgu = 0;
  for (uint32_t i = 0; i < pendingAguCount; i++) {
    if (pendingAgu[i].seq <= keepSeq)
      pendingAgu[keptAgu++] = pendingAgu[i];
  }
  pendingAguCount = keptAgu;

  while (lsq.count() && lsq.tail().seq > keepSeq) {
    const uint32_t tailSlot = lsq.indexOf(lsq.count() - 1);
    const LsqEntry &entry = lsq.at(tailSlot);
    if (entry.isStore) {
      if (!entry.addrValid)
        lsqUnknownStores--;
      if (!entry.dataReady) {
        lsqStoresWaitingData--;
        if (lsqUseWaitMask)
          storeDataWaitMask[entry.dataPhysReg] &= ~(1ull << tailSlot);
      }
    } else {
      if (entry.addrValid && !entry.issued && !entry.done)
        lsqReadyLoads--;
      if (entry.issued || entry.done)
        lsqSpecIssuedLoads--;
      loadOutputs[tailSlot].valid = false;
    }
    lsq.popTail();
  }

  uint32_t keptWb = 0;
  wbBranchCount = 0;
  for (uint32_t i = 0; i < wbReadyCount; i++) {
    if (wbReady[i]->valid) {
      wbReady[keptWb++] = wbReady[i];
      if (rob.at(wbReady[i]->robIdx).isBranch)
        wbBranchCount++;
    }
  }
  wbReadyCount = keptWb;

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
  const uint32_t count = pendingAguCount;
  pendingAguCount = 0;
  bool flushed = false;

  for (uint32_t i = 0; i < count; i++) {
    const PendingAgu &op = pendingAgu[i];
    if (flushed && (!lsq.live(op.lsqIdx) || lsq.at(op.lsqIdx).seq != op.seq))
      continue;

    LsqEntry &lsqEntry = lsq.at(op.lsqIdx);
    lsqEntry.addr = op.addr;
    lsqEntry.addrValid = true;

    RobEntry &robEntry = rob.at(op.robIdx);
    if ((lsqEntry.addr & (lsqEntry.size - 1)) != 0)
      robEntry.fault = 1;
    else if ((uint64_t)lsqEntry.addr + lsqEntry.size > memSystem.backing.bytes.size())
      robEntry.fault = 2;

    if (lsqEntry.isStore) {
      lsqUnknownStores--;
      if (lsqEntry.dataReady)
        robEntry.done = true;

      if (config.memOrder == MemOrder::Speculative && !robEntry.fault &&
          lsqSpecIssuedLoads > 0) {
        const uint64_t prevFlushes = stats.flushes;
        violationScan(lsqEntry);
        if (stats.flushes != prevFlushes)
          flushed = true;
      }
    } else {
      lsqReadyLoads++;
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

void OoOCore::reportLoad(uint32_t lsqSlot, LsqEntry &entry) {
  FuOp &slot = loadOutputs[lsqSlot];
  slot.valid = true;
  slot.seq = entry.seq;
  slot.robIdx = (uint16_t)entry.robIdx;
  slot.destPhysReg = entry.destPhysReg;
  slot.value = entry.value;
  wbReady[wbReadyCount++] = &slot;
  entry.reported = true;
}

void OoOCore::drainDataResponses() {
  auto hasResp = [&]() { return l1dCache ? l1dCache->hasResponse() : dmem->hasResponse(); };

  while (hasResp()) {
    const MemResponse &response =
        l1dCache ? l1dCache->frontResponse() : dmem->frontResponse();
    const uint64_t tag = response.tag;
    const uint32_t data = response.readData;

    if (l1dCache)
      l1dCache->popResponse();
    else
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
    reportLoad(lsqSlot, lsqEntry);
  }
}

// Captures ready store data, checks store-to-load forwarding, issues up to
// `dataPorts` accesses to L1D, and reports completed loads to writeback.
void OoOCore::lsqOperate() {
  if (!flatFastPath)
    drainDataResponses();

  while (!storeBuffer.empty() && storeBuffer.head().acked)
    storeBuffer.popHead();

  if (lsq.empty() && storeBuffer.empty())
    return;

  // Fallback store-data scan when lsqSize > 64 (smaller LSQs wake in writebackStage).
  if (!lsqUseWaitMask && lsqStoresWaitingData) {
    for (uint32_t i = 0; i < lsq.count(); i++) {
      LsqEntry &entry = lsq.nth(i);
      if (entry.isStore && !entry.dataReady && physRegs.ready[entry.dataPhysReg]) {
        entry.data = physRegs.values[entry.dataPhysReg];
        entry.dataReady = true;
        if (entry.addrValid)
          rob.at(entry.robIdx).done = true;
        if (--lsqStoresWaitingData == 0)
          break;
      }
    }
  }

  if (lsqReadyLoads == 0 && !storeBuffer.nextToIssue())
    return;

  uint64_t sbStoreMask = 0;
  if (lsqReadyLoads > 0) {
    for (uint32_t j = 0; j < storeBuffer.count(); j++) {
      const StoreBufferEntry &sbEntry = storeBuffer.nth(j);
      sbStoreMask |= wordBucketMask(sbEntry.addr, sbEntry.size);
    }
  }

  uint32_t searchStart = 0;
  uint64_t olderStoreMask = 0;
  bool loadsExhausted = (lsqReadyLoads == 0);

  for (uint32_t port = 0; port < config.dataPorts; ++port) {
    LsqEntry *selectedLoad = nullptr;
    uint32_t selectedSlot = 0;

    if (!loadsExhausted && lsqReadyLoads > 0) {
      bool foundLoad = false;

      for (uint32_t i = searchStart; i < lsq.count(); i++) {
        LsqEntry &entry = lsq.nth(i);

        if (entry.isStore) {
          if (!entry.addrValid) {
            if (config.memOrder != MemOrder::Speculative) {
              loadsExhausted = true;
              break;
            }
          } else {
            olderStoreMask |= wordBucketMask(entry.addr, entry.size);
          }
          continue;
        }

        if (!entry.addrValid || entry.done)
          continue;

        if (entry.issued) {
          if (config.memOrder == MemOrder::Conservative) {
            loadsExhausted = true;
            break;
          }
          continue;
        }

        // Faulting loads skip memory and complete immediately so commit raises the fault.
        if (rob.at(entry.robIdx).fault) {
          entry.value = 0;
          entry.done = true;
          lsqReadyLoads--;
          lsqSpecIssuedLoads++;
          reportLoad(lsq.indexOf(i), entry);
          continue;
        }

        bool blocked = false;
        bool forwarded = false;
        const uint64_t loadMask = wordBucketMask(entry.addr, entry.size);

        auto checkOlderStore = [&](uint32_t storeAddr, uint32_t storeSize,
                                   bool storeDataReady, uint32_t storeData) {
          if (storeAddr == entry.addr && storeSize == entry.size) {
            if (!storeDataReady) {
              blocked = true;
            } else {
              entry.value = extendLoad(entry.op, storeData);
              entry.done = true;
              forwarded = true;
            }
          } else if (overlaps(storeAddr, storeSize, entry.addr, entry.size)) {
            blocked = true;
          }
        };

        // Search youngest-to-oldest older stores only when the word-bucket
        // signature indicates a possible overlap.
        if (olderStoreMask & loadMask) {
          for (int j = (int)i - 1; j >= 0 && !blocked && !forwarded; j--) {
            const LsqEntry &storeEntry = lsq.nth((uint32_t)j);
            if (storeEntry.isStore && storeEntry.addrValid) {
              checkOlderStore(storeEntry.addr, storeEntry.size,
                              storeEntry.dataReady, storeEntry.data);
            }
          }
        }

        if (!blocked && !forwarded && (sbStoreMask & loadMask)) {
          for (int j = (int)storeBuffer.count() - 1; j >= 0 && !blocked && !forwarded; j--) {
            const StoreBufferEntry &storeEntry = storeBuffer.nth((uint32_t)j);
            checkOlderStore(storeEntry.addr, storeEntry.size, true, storeEntry.data);
          }
        }

        if (forwarded) {
          stats.loadsForwarded++;
          lsqReadyLoads--;
          lsqSpecIssuedLoads++;
          reportLoad(lsq.indexOf(i), entry);
          if (config.memOrder == MemOrder::Conservative) {
            loadsExhausted = true;
            break;
          }
          continue;
        }

        if (!blocked) {
          selectedLoad = &entry;
          selectedSlot = lsq.indexOf(i);
          searchStart = i;
          foundLoad = true;
          break;
        }

        if (config.memOrder == MemOrder::Conservative) {
          loadsExhausted = true;
          break;
        }
      }

      if (!foundLoad)
        loadsExhausted = true;
    }

    bool sentAccess = false;
    StoreBufferEntry *storeEntry = storeBuffer.nextToIssue();

    if (flatFastPath) {
      if (storeEntry && (storeBuffer.full() || !selectedLoad)) {
        writeLittleEndian(memSystem.backing.bytes.data() + storeEntry->addr,
                          storeEntry->size, storeEntry->data);
        storeEntry->acked = true;
        storeBuffer.issue();
        sentAccess = true;
      } else if (selectedLoad) {
        const uint32_t rawData = readLittleEndian(
            memSystem.backing.bytes.data() + selectedLoad->addr, selectedLoad->size);
        selectedLoad->value = extendLoad(selectedLoad->op, rawData);
        selectedLoad->issued = true;
        selectedLoad->done = true;
        lsqReadyLoads--;
        lsqSpecIssuedLoads++;
        reportLoad(selectedSlot, *selectedLoad);
        searchStart++;
        sentAccess = true;
      }
    } else {
      const bool portReady = l1dCache ? l1dCache->canAccept() : dmem->canAccept();

      // Prioritize draining the store buffer when full or when no load is ready.
      if (storeEntry && (storeBuffer.full() || !selectedLoad)) {
        if (portReady) {
          uint32_t unusedRead = 0;
          if (l1dCache && l1dCache->tryAccessWordImmediate(
                              storeEntry->addr, storeEntry->size, true,
                              storeEntry->data, unusedRead)) {
            storeEntry->acked = true;
            storeBuffer.issue();
          } else {
            const uint64_t tag = makeStoreTag(storeBuffer.nextIssueSlot(), storeBufferTxnCounter);
            storeEntry->txnId = storeBufferTxnCounter++;
            storeBuffer.issue();

            if (l1dCache) {
              l1dCache->accessWord(storeEntry->addr, storeEntry->size, true,
                                   storeEntry->data, tag);
            } else {
              MemRequest req;
              req.addr = storeEntry->addr;
              req.size = storeEntry->size;
              req.isWrite = true;
              req.writeData = storeEntry->data;
              req.tag = tag;
              dmem->access(req);
            }
          }
          sentAccess = true;
        }
      } else if (selectedLoad && portReady) {
        selectedLoad->issued = true;
        lsqReadyLoads--;
        lsqSpecIssuedLoads++;

        uint32_t hitData = 0;
        if (l1dCache && l1dCache->tryAccessWordImmediate(
                            selectedLoad->addr, selectedLoad->size, false, 0,
                            hitData)) {
          selectedLoad->value = extendLoad(selectedLoad->op, hitData);
          selectedLoad->done = true;
          reportLoad(selectedSlot, *selectedLoad);
        } else {
          const uint64_t tag = makeLoadTag(selectedSlot, selectedLoad->generation);
          if (l1dCache) {
            l1dCache->accessWord(selectedLoad->addr, selectedLoad->size, false, 0, tag);
          } else {
            MemRequest req;
            req.addr = selectedLoad->addr;
            req.size = selectedLoad->size;
            req.tag = tag;
            dmem->access(req);
          }
          if (config.memOrder == MemOrder::Conservative)
            loadsExhausted = true;
        }
        searchStart++;
        sentAccess = true;
      }
    }

    if (!sentAccess)
      break;
  }

  if (!flatFastPath)
    drainDataResponses();
}

bool OoOCore::tryIssueEntry(const IqEntry &entry, size_t &nextAlu,
                            size_t &nextBranch) {
  if (entry.unit == FuKind::LSU) {
    if (pendingAguCount >= config.aguCount)
      return false;

    const RobEntry &robEntry = rob.at(entry.robIdx);
    const uint32_t addr =
        physRegs.values[entry.src1PhysReg] + (uint32_t)robEntry.instr.imm;
    pendingAgu[pendingAguCount++] =
        PendingAgu{entry.seq, addr, entry.robIdx, entry.lsqIdx};
    return true;
  }

  FuUnit *targetUnit = nullptr;
  uint32_t unitIdx = 0;
  bool oneCycleDirect = false;

  switch (entry.unit) {
  case FuKind::ALU:
    while (nextAlu < alus.size() && !alus[nextAlu].canAccept())
      nextAlu++;
    if (nextAlu < alus.size()) {
      unitIdx = (uint32_t)nextAlu;
      targetUnit = &alus[nextAlu++];
      oneCycleDirect = !targetUnit->output.valid;
    }
    break;

  case FuKind::BRANCH:
    while (nextBranch < branchUnits.size() && !branchUnits[nextBranch].canAccept())
      nextBranch++;
    if (nextBranch < branchUnits.size()) {
      unitIdx = branchExecBegin + (uint32_t)nextBranch;
      targetUnit = &branchUnits[nextBranch++];
      oneCycleDirect = !targetUnit->output.valid;
    }
    break;

  case FuKind::MUL:
    if (mulUnit.canAccept()) {
      unitIdx = mulExecIdx;
      targetUnit = &mulUnit;
    }
    break;

  case FuKind::DIV:
    if (divUnit.canAccept()) {
      unitIdx = divExecIdx;
      targetUnit = &divUnit;
    }
    break;

  default:
    break;
  }

  if (!targetUnit)
    return false;

  const RobEntry &robEntry = rob.at(entry.robIdx);
  const ExecResult execResult = execute(
      robEntry.instr, robEntry.pc, physRegs.values[entry.src1PhysReg],
      physRegs.values[entry.src2PhysReg]);

  if (oneCycleDirect) {
    FuOp &out = targetUnit->output;
    out.valid = true;
    out.seq = entry.seq;
    out.robIdx = entry.robIdx;
    out.lsqIdx = entry.lsqIdx;
    out.destPhysReg = robEntry.destPhysReg;
    out.value = execResult.value;
    out.redirect = execResult.redirect;
    out.target = execResult.target;
    wbReady[wbReadyCount++] = &out;
    if (entry.unit == FuKind::BRANCH)
      wbBranchCount++;
    return true;
  }

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
  activeExecMask |= (1ull << unitIdx);
  return true;
}

// Issues up to `width` ready instructions oldest-first to available functional units.
void OoOCore::issueStage() {
  uint32_t issued = 0;
  size_t nextAlu = 0;
  size_t nextBranch = 0;

  if (iq.usesBitmask()) {
    const uint32_t total = iq.entryCount;
    uint32_t kept = 0;

    for (uint32_t i = 0; i < total; i++) {
      const uint8_t slot = iq.ageOrder[i];
      if ((iq.readyMask & (1ull << slot)) &&
          tryIssueEntry(iq.entries[slot], nextAlu, nextBranch)) {
        iq.releaseSlot(slot);
        issued++;

        if (issued == config.width || iq.readyCount == 0) {
          for (uint32_t j = i + 1; j < total; j++)
            iq.ageOrder[kept++] = iq.ageOrder[j];
          iq.entryCount = kept;
          return;
        }
        continue;
      }

      if (kept != i)
        iq.ageOrder[kept] = slot;
      kept++;
    }

    iq.entryCount = kept;
    return;
  }

  const size_t total = iq.entries.size();
  size_t kept = 0;

  for (size_t i = 0; i < total; i++) {
    IqEntry &entry = iq.entries[i];

    if (entry.ready1 && entry.ready2 && tryIssueEntry(entry, nextAlu, nextBranch)) {
      iq.readyCount--;
      issued++;

      if (issued == config.width || iq.readyCount == 0) {
        for (size_t j = i + 1; j < total; j++)
          iq.entries[kept++] = iq.entries[j];
        iq.entries.resize(kept);
        return;
      }
      continue;
    }

    if (kept != i)
      iq.entries[kept] = entry;
    kept++;
  }

  if (kept != total)
    iq.entries.resize(kept);
}

// Decodes, renames, and allocates ROB, IQ, and LSQ entries up to `width` per cycle.
void OoOCore::dispatchStage() {
  for (uint32_t lane = 0; lane < config.width; lane++) {
    if (fetchQueue.empty())
      return;

    const FetchedInstr &fetched = fetchQueue.head();
    const DecodedEntry &decoded = decodeCached(fetched.pc, fetched.raw);
    const Instr &instr = decoded.instr;
    const FuKind unitKind = decoded.unit;
    const bool isSystemOp = unitKind == FuKind::NONE;
    const bool isMemOp = unitKind == FuKind::LSU;
    const bool hasDest = (decoded.flags & 1u) != 0;

    // System ops (ECALL, EBREAK, FENCE, ILLEGAL) serialize on an empty machine.
    if (isSystemOp && (lane != 0 || !rob.empty() || !storeBuffer.empty()))
      return;
    if (rob.full() || (isMemOp && lsq.full()) || iq.full() || (hasDest && freeList.empty()))
      return;

    const uint32_t fetchedPc = fetched.pc;
    const bool predTaken = fetched.predTaken;
    const uint32_t predTarget = fetched.predTarget;
    const uint32_t phtIndex = fetched.phtIndex;
    const bool storeOp = (decoded.flags & 8u) != 0;
    const bool needsCheckpoint =
        unitKind == FuKind::BRANCH ||
        (isMemOp && !storeOp && config.memOrder == MemOrder::Speculative);
    Predictor::Checkpoint predCheckpoint;
    if (needsCheckpoint)
      predCheckpoint = fetched.predCheckpoint;
    fetchQueue.popHead();

    const uint64_t seq = nextSeq++;
    const uint32_t robIdx = rob.allocRaw();
    RobEntry &robEntry = rob.at(robIdx);
    robEntry.seq = seq;
    robEntry.pc = fetchedPc;
    robEntry.instr = instr;
    robEntry.done = false;
    robEntry.destPhysReg = kNoReg;
    robEntry.prevPhysReg = kNoReg;
    robEntry.isMem = false;
    robEntry.isSystem = false;
    robEntry.fault = 0;
    robEntry.isBranch = false;
    if (needsCheckpoint)
      robEntry.predCheckpoint = predCheckpoint;

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
      lsqIdx = lsq.allocRaw();
      LsqEntry &lsqEntry = lsq.at(lsqIdx);
      lsqEntry.seq = seq;
      lsqEntry.robIdx = robIdx;
      lsqEntry.pc = fetchedPc;
      lsqEntry.op = instr.op;
      lsqEntry.size = decoded.memSize;
      lsqEntry.isStore = storeOp;
      lsqEntry.addrValid = false;
      lsqEntry.done = false;
      lsqEntry.issued = false;
      lsqEntry.reported = false;
      lsqEntry.destPhysReg = robEntry.destPhysReg;

      if (storeOp) {
        lsqUnknownStores++;
        lsqEntry.dataPhysReg = src2PhysReg;
        lsqEntry.dataReady = physRegs.ready[src2PhysReg];
        if (lsqEntry.dataReady) {
          lsqEntry.data = physRegs.values[src2PhysReg];
        } else {
          lsqStoresWaitingData++;
          if (lsqUseWaitMask)
            storeDataWaitMask[src2PhysReg] |= (1ull << lsqIdx);
        }
      } else {
        lsqEntry.dataReady = true;
      }
    }

    if (unitKind == FuKind::BRANCH) {
      robEntry.isBranch = true;
      robEntry.predictedTaken = predTaken;
      robEntry.predictedTarget = predTarget;
      robEntry.actualTaken = false;
      robEntry.actualTarget = 0;
      robEntry.phtIndex = phtIndex;
    }

    // Store data (rs2) is captured directly in the LSQ, so the AGU only waits on rs1.
    const bool usesSrc1 = (decoded.flags & 2u) != 0;
    const bool usesSrc2 = (decoded.flags & 4u) != 0;
    IqEntry iqEntry;
    iqEntry.seq = seq;
    iqEntry.robIdx = (uint16_t)robIdx;
    iqEntry.lsqIdx = (uint16_t)lsqIdx;
    iqEntry.src1PhysReg = src1PhysReg;
    iqEntry.src2PhysReg = src2PhysReg;
    iqEntry.ready1 = !usesSrc1 || physRegs.ready[src1PhysReg];
    iqEntry.ready2 = !usesSrc2 || storeOp || physRegs.ready[src2PhysReg];
    iqEntry.unit = unitKind;
    iq.push(iqEntry);
  }
}

void OoOCore::fetchStage() {
  if (flatFastPath) {
    if (pendingRedirect || fetchQueue.count() + fetchBlockBytes / 4 > config.fetchQSize)
      return;

    if (pc & 3u)
      return;

    if (pc >= memSystem.backing.bytes.size()) {
      if (rob.empty() && fetchQueue.count() == 0)
        memSystem.backing.failOutOfRange(pc, "instruction fetch");
      return;
    }

    fetchBlockAddr = pc & ~(fetchBlockBytes - 1);
    fetchStartPc = pc;
    memSystem.backing.check(fetchBlockAddr, fetchBlockBytes, "load");
    processFetch(memSystem.backing.bytes.data() + fetchBlockAddr);
    return;
  }

  auto consumeResponse = [&] {
    const MemResponse &response =
        l1iCache ? l1iCache->frontResponse() : imem->frontResponse();
    fetchOutstanding = false;

    if (fetchStale)
      fetchStale = false;
    else
      processFetch(response.readLine.data());

    if (l1iCache)
      l1iCache->popResponse();
    else
      imem->popResponse();
  };

  const bool hasResp = l1iCache ? l1iCache->hasResponse() : imem->hasResponse();
  if (fetchOutstanding && hasResp)
    consumeResponse();

  if (fetchOutstanding || pendingRedirect ||
      fetchQueue.count() + fetchBlockBytes / 4 > config.fetchQSize) {
    return;
  }

  // Wrong-path speculative PCs may be misaligned or out of range; stall fetch
  // until the older branch redirects or the machine drains and faults precisely.
  if (pc & 3u)
    return;

  if (pc >= memSystem.backing.bytes.size()) {
    if (rob.empty() && fetchQueue.count() == 0)
      memSystem.backing.failOutOfRange(pc, "instruction fetch");
    return;
  }

  const bool canReq = l1iCache ? l1iCache->canAccept() : imem->canAccept();
  if (!canReq)
    return;

  const uint32_t blockAddr = pc & ~(fetchBlockBytes - 1);
  fetchBlockAddr = blockAddr;
  fetchStartPc = pc;

  if (l1iCache) {
    if (const uint8_t *linePtr = l1iCache->tryFetchLineImmediate(blockAddr)) {
      processFetch(linePtr);
      return;
    }
  }

  MemRequest req;
  req.addr = blockAddr;
  req.size = fetchBlockBytes;

  if (l1iCache)
    l1iCache->access(req);
  else
    imem->access(req);

  fetchOutstanding = true;

  if (l1iCache ? l1iCache->hasResponse() : imem->hasResponse())
    consumeResponse();
}

void OoOCore::processFetch(const uint8_t *blockData) {
  const uint32_t startWord = (fetchStartPc - fetchBlockAddr) >> 2;
  const uint32_t blockWords = fetchBlockBytes >> 2;
  const bool needLoadCheckpoint =
      config.usePredictor && config.memOrder == MemOrder::Speculative;

  for (uint32_t wordIdx = startWord; wordIdx < blockWords; wordIdx++) {
    const uint32_t rawWord = readLittleEndian(blockData + wordIdx * 4, 4);
    const uint32_t instrPc = fetchBlockAddr + wordIdx * 4;
    FetchedInstr &slot = fetchQueue.at(fetchQueue.allocRaw());
    slot.pc = instrPc;
    slot.raw = rawWord;

    if (config.usePredictor) {
      const uint32_t opcode = rawWord & 0x7Fu;

      // Only RV32IM control-transfer opcodes (BRANCH, JAL, JALR) can exist in
      // the BTB or update branch history; skip BTB/PHT probes for all others.
      if (opcode == 0x63u || opcode == 0x6Fu || opcode == 0x67u) {
        const Predictor::Prediction prediction = predictor.predict(instrPc);
        slot.predTaken = prediction.taken;
        slot.predTarget = prediction.target;
        slot.phtIndex = prediction.phtIndex;
        slot.predCheckpoint = prediction.before;

        if (prediction.taken) {
          pc = prediction.target;
          return;
        }
        continue;
      }

      if (needLoadCheckpoint && opcode == 0x03u)
        slot.predCheckpoint = predictor.checkpoint();
    } else {
      slot.predTaken = false;
      slot.predTarget = 0;
    }
  }

  pc = fetchBlockAddr + fetchBlockBytes;
}

void OoOCore::stepCycle() {
  stats.cycles++;

  commitStage();
  if (halted) [[unlikely]]
    return;

  if (wbReadyCount)
    writebackStage();

  if (pendingAguCount)
    aguDrain();

  lsqOperate();

  if (iq.readyCount)
    issueStage();

  if (!fetchQueue.empty())
    dispatchStage();

  if (pendingRedirect) [[unlikely]] {
    if (fetchOutstanding)
      fetchStale = true;
    pc = pendingTarget;
    pendingRedirect = false;
  }

  fetchStage();

  uint64_t execMask = activeExecMask;
  while (execMask) {
    const uint32_t idx = (uint32_t)std::countr_zero(execMask);
    execMask &= execMask - 1;
    FuUnit *unit = execUnits[idx];

    if (unit->tick()) {
      wbReady[wbReadyCount++] = &unit->output;
      if (idx >= branchExecBegin && idx < branchExecEnd)
        wbBranchCount++;
    }
    if (!unit->busy())
      activeExecMask &= ~(1ull << idx);
  }

  if (!flatFastPath) {
    memSystem.tick();
    drainDataResponses();
  }

  while (!storeBuffer.empty() && storeBuffer.head().acked)
    storeBuffer.popHead();

  if (config.trace) [[unlikely]] {
    fprintf(stderr, "cyc %6" PRIu64 " | pc=0x%08x fq=%u rob=%u iq=%u lsq=%u sb=%u\n",
            stats.cycles, pc, fetchQueue.count(), rob.count(), iq.count(),
            lsq.count(), storeBuffer.count());
  }
}
