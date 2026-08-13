#include "core/cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isa/execute.h"

CPU::CPU(const SimConfig &cfg, MemorySystem &msys)
    : msys(msys), imem(msys.imem), dmem(msys.dmem),
      showMemStats(!cfg.flatMemory || cfg.flatLatency > 1), trace(cfg.trace),
      maxCycles(cfg.maxCycles) {
  memset(regs, 0, sizeof regs);
}

void CPU::loadWords(const std::vector<uint32_t> &words) {
  for (size_t i = 0; i < words.size(); i++) {
    msys.backing.store32((uint32_t)(i * 4), words[i]);
  }
}

void CPU::loadBytes(const std::vector<uint8_t> &bytes) {
  msys.backing.check(0, (uint32_t)bytes.size(), "program image");
  memcpy(msys.backing.bytes.data(), bytes.data(), bytes.size());
}

int CPU::run() {
  while (!halted) {
    if (stats.cycles >= maxCycles) {
      fprintf(stderr,
              "stopping after %" PRIu64
              " cycles without an exit syscall (raise with -c)\n",
              stats.cycles);
      exitCode = 2;
      break;
    }
    stepCycle();
  }
  stats.printCore();
  if (showMemStats) {
    stats.printMemStalls();
    msys.printStats(stats.retired);
  }
  stats.printExit(exitCode);
  return exitCode;
}

void CPU::dumpRegs() const {
  fprintf(stderr, "--- registers ---\n");
  for (int i = 0; i < 32; i++) {
    fprintf(stderr, "%4s=%08x%s", kRegName[i], regs[i],
            (i % 4 == 3) ? "\n" : "  ");
  }
  fprintf(stderr, "pc  =%08x\n", pc);
}

// The fetch unit (IF stage). Runs every cycle, even while MEM holds the
// rest of the pipeline: it drains a completed fetch into the one-entry
// buffer (dropping it if a redirect made it stale) and issues a fetch
// for pc as soon as it has nothing in flight and nothing buffered. pc
// advances at issue; a redirect rewrites it and empties the buffer
void CPU::updateFetch() {
  if (fOutstanding && imem->done()) {
    MemResponse r = imem->response();
    fOutstanding = false;
    if (fStale)
      fStale = false; // wrong-path fetch: drop it
    else {
      fBufValid = true;
      fBufPC = fPC;
      fBufRaw = r.rdata;
    }
  }
  if (!fOutstanding && !fBufValid) {
    if (pc % 4 != 0) {
      fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
      exit(1);
    }
    if (imem->canAccept()) {
      MemRequest req;
      req.addr = pc;
      req.size = 4;
      imem->access(req);
      fOutstanding = true;
      fPC = pc;
      pc += 4;
      if (imem->done()) { // 1-cycle port: complete within this cycle
        MemResponse r = imem->response();
        fOutstanding = false;
        fBufValid = true;
        fBufPC = fPC;
        fBufRaw = r.rdata;
      } else {
        events.push_back("ifetch wait");
      }
    }
  }
}

// ID - instruction decode stage: decode instruction, read registers,
//      detect the load-use hazard
IDEX CPU::doID(bool &stall) {
  IDEX out;
  if (!ifid.valid)
    return out;

  const Instr I = decode(ifid.raw);

  // Hazard detection unit. If the instruction currently in EX (i.e., the
  // one in the ID/EX latch right now) is a load whose destination this
  // instruction needs in EX next cycle, we must stall to prevent a load-use
  // hazard. The load's data is only available after MEM, one cycle too late
  // for any forwarding path to save us.
  if ((idex.valid && isLoad(idex.ins.op) && idex.ins.rd != 0) &&
      ((usesRs1(I.op) && I.rs1 == idex.ins.rd) ||
       (rs2NeededInEX(I.op) && I.rs2 == idex.ins.rd))) {
    stall = true;
    return out; // default-constructed IDEX is a bubble
  }

  out.valid = true;
  out.pc = ifid.pc;
  out.ins = I;
  // Register file read. WB already ran this cycle, so a producer
  // three instructions ahead needs no forwarding at all
  out.rs1val = regs[I.rs1];
  out.rs2val = regs[I.rs2];
  return out;
}

// Operand forwarding. The instruction in EX/MEM is younger than the one in
// MEM/WB, so its value must win when both match. Loads are excluded as EX/MEM
// sources because their data does not exist until the end of MEM.
uint32_t CPU::fwd(uint8_t reg, uint32_t valueFromID) const {
  if (reg == 0)
    return 0; // x0 never forwards
  if (exmem.valid && writesRd(exmem.ins.op) && !isLoad(exmem.ins.op) &&
      exmem.ins.rd == reg) {
    return exmem.aluResult;
  }
  if (memwb.valid && writesRd(memwb.ins.op) && memwb.ins.rd == reg) {
    return memwb.result;
  }
  return valueFromID; // no in-flight producer, the ID read is good
}

// EX - execute stage: resolve operands through the forwarding network,
//      then apply the pure execute semantics from isa/execute.h.
//      A real core would be multicycle for mul and div; we charge 1 cycle
//      like everything else for now.
EXMEM CPU::doEX(bool &redirect, uint32_t &redirectPC) {
  EXMEM out;
  if (!idex.valid)
    return out;
  const Instr &I = idex.ins;

  out.valid = true;
  out.pc = idex.pc;
  out.ins = I;

  // Resolve source operands through the forwarding network
  const uint32_t a = usesRs1(I.op) ? fwd(I.rs1, idex.rs1val) : 0;
  const uint32_t b = usesRs2(I.op) ? fwd(I.rs2, idex.rs2val) : 0;

  const ExecResult r = execute(I, idex.pc, a, b);
  out.aluResult = r.value;
  out.storeData = b; // forwarded rs2, carried along for MEM
  redirect = r.redirect;
  redirectPC = r.target;

  return out;
}

// RV32I permits trapping on misaligned data accesses, but we
// treat them as fatal since we don't model trap handling
void CPU::checkAlign(Op op, uint32_t addr, uint32_t atPc) {
  uint32_t need = 1;
  if (op == Op::LH || op == Op::LHU || op == Op::SH)
    need = 2;
  if (op == Op::LW || op == Op::SW)
    need = 4;
  if (addr % need != 0) {
    fprintf(stderr, "fatal: misaligned %s of 0x%08x at pc=0x%08x\n",
            isStore(op) ? "store" : "load", addr, atPc);
    exit(1);
  }
}

// MEM - data memory stage. Non-memory instructions pass through in one
// cycle. Loads and stores issue to the data port on their first cycle
// in the stage — capturing store-data forwarding from the current
// MEM/WB latch at that instant, which always holds the store's
// immediate predecessor — and complete when the port answers. memDone
// is false while the answer is outstanding; the pipeline holds
MEMWB CPU::doMEM(bool &memDone) {
  MEMWB out;
  memDone = true;
  if (!exmem.valid)
    return out;
  const Instr &I = exmem.ins;

  out.valid = true;
  out.pc = exmem.pc;
  out.ins = I;
  out.result = exmem.aluResult;

  if (!isLoad(I.op) && !isStore(I.op))
    return out;

  if (!dOutstanding) {
    const uint32_t addr = exmem.aluResult;
    checkAlign(I.op, addr, exmem.pc);
    uint32_t size = 4;
    if (I.op == Op::LB || I.op == Op::LBU || I.op == Op::SB)
      size = 1;
    else if (I.op == Op::LH || I.op == Op::LHU || I.op == Op::SH)
      size = 2;

    MemRequest req;
    req.addr = addr;
    req.size = size;
    req.isWrite = isStore(I.op);
    if (req.isWrite) {
      uint32_t data = exmem.storeData;
      // MEM/WB -> MEM store-data forwarding. Covers the case "lw x1 / sw
      // x1": when the store sat in EX, the load's data did not exist yet,
      // so the value carried in storeData is stale; now the load is in WB
      // and its data is in the MEM/WB latch. The instruction in WB is
      // always the store's immediate predecessor (it completed MEM before
      // the store entered), so its value is never outdated
      if (memwb.valid && writesRd(memwb.ins.op) && memwb.ins.rd != 0 &&
          memwb.ins.rd == I.rs2) {
        data = memwb.result;
      }
      req.wdata = data;
      dVal = (size == 4) ? data : (data & ((1u << (8 * size)) - 1));
    }
    if (!dmem->canAccept()) {
      // Cannot happen in the blocking design: MEM is the port's only
      // requester and consumed its previous response before reissuing
      fprintf(stderr, "internal error: data port busy at issue\n");
      exit(1);
    }
    dmem->access(req);
    dOutstanding = true;
    dAddr = addr;
    dSize = (uint8_t)size;
    if (!dmem->done())
      events.push_back("dmem wait");
  }

  if (!dmem->done()) {
    memDone = false;
    return out;
  }

  MemResponse resp = dmem->response();
  dOutstanding = false;
  if (isLoad(I.op)) {
    switch (I.op) {
    // Sub-word loads sign- or zero-extended into 32 bits as per the ISA
    case Op::LB:
      out.result = (uint32_t)(int32_t)(int8_t)resp.rdata;
      break;
    case Op::LBU:
      out.result = resp.rdata & 0xFF;
      break;
    case Op::LH:
      out.result = (uint32_t)(int32_t)(int16_t)resp.rdata;
      break;
    case Op::LHU:
      out.result = resp.rdata & 0xFFFF;
      break;
    default:
      out.result = resp.rdata;
      break;
    }
  } else {
    out.memWrite = MemoryWrite{dAddr, dVal, dSize};
  }
  return out;
}

// WB - write-back stage. Retirement happens here and nowhere else, so
// this is also where each instruction's CommitRecord is emitted
void CPU::doWB() {
  if (!memwb.valid)
    return;
  const Instr &I = memwb.ins;

  CommitRecord rec;
  rec.sequence = stats.retired;
  rec.pc = memwb.pc;
  rec.instruction = I.raw;
  rec.memoryWrite = memwb.memWrite; // recorded by MEM, if a store

  switch (I.op) {
  case Op::ECALL:
    stats.retired++;
    doSyscall();
    if (onCommit)
      onCommit(rec);
    return;
  case Op::EBREAK:
    stats.retired++;
    fprintf(stderr, "ebreak at pc=0x%08x — halting\n", memwb.pc);
    halted = true;
    if (onCommit)
      onCommit(rec);
    return;
  case Op::ILLEGAL:
    // We let illegal words flow down the pipe and trap them only here,
    // so that wrong-path garbage fetched after a taken branch (which
    // gets squashed long before WB) never causes a spurious error.
    fprintf(stderr, "illegal instruction 0x%08x at pc=0x%08x\n", I.raw,
            memwb.pc);
    halted = true;
    exitCode = 1;
    rec.exception = Exception{ExceptionKind::IllegalInstruction};
    if (onCommit)
      onCommit(rec);
    return;
  default:
    break;
  }

  // The one and only architectural register write
  if (writesRd(I.op) && I.rd != 0) {
    regs[I.rd] = memwb.result;
    rec.registerWrite = RegisterWrite{I.rd, memwb.result};
  }
  stats.retired++;
  if (onCommit)
    onCommit(rec);
}

// Minimal syscall interface (a7 = number, a0 = argument)
//   1  print a0 as a signed decimal integer, followed by a newline
//   2  print a0 as a single ASCII character
//   3  print the NUL-terminated string at address a0
//   93 exit with code a0  (Linux-flavoured number; 10 also accepted)
void CPU::doSyscall() {
  const uint32_t num = regs[17]; // a7
  const uint32_t arg = regs[10]; // a0
  switch (num) {
  case 1:
    printf("%d\n", (int32_t)arg);
    break;
  case 2:
    putchar((int)(arg & 0xFF));
    break;
  case 3:
    // Reads snoop the caches (peek8): with write-back caches the newest
    // copy of a byte may not have reached the backing store yet
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
            memwb.pc);
    break;
  }
}

// One clock cycle
//
// Evaluation order matters for these intra-cycle dependencies we model:
//      1. WB writes the register file BEFORE ID reads it
//      2. MEM reads the current MEM/WB latch for store-data forwarding,
//         and EX reads the current EX/MEM and MEM/WB latches for ALU
//         forwarding, hence, all stages compute their output into local
//         "next" latches which are committed together at the end
//      3. If MEM's port has not answered yet, only WB advances (it
//         receives a bubble); every older latch and the fetch buffer
//         hold, which preserves the relative stage positions that the
//         forwarding invariants above rely on
void CPU::stepCycle() {
  stats.cycles++;
  events.clear();
  if (trace)
    captureStageView();

  // WB first
  doWB();
  if (halted) {
    if (trace)
      printTrace();
    return;
  }

  // compute next state of every latch from current state
  bool memDone = true;
  MEMWB nMemwb = doMEM(memDone);

  if (!memDone) {
    // MEM is waiting on the data port: bubble into WB, hold the rest.
    // The fetch unit keeps running — it can fill its buffer meanwhile
    stats.dataStallCycles++;
    // The frozen EX instruction's operands were read from the register
    // file back in ID; its producer is about to drain out of the
    // forwarding window (WB already wrote the register file this cycle,
    // and memwb becomes a bubble below). Capture the forwarded values
    // into the ID/EX latch while they are still visible, so EX computes
    // with fresh operands when the stall ends. Idempotent on later
    // stall cycles: with memwb a bubble, fwd() returns the latch value
    if (idex.valid) {
      if (usesRs1(idex.ins.op))
        idex.rs1val = fwd(idex.ins.rs1, idex.rs1val);
      if (usesRs2(idex.ins.op))
        idex.rs2val = fwd(idex.ins.rs2, idex.rs2val);
    }
    memwb = MEMWB{};
    updateFetch();
    msys.tick();
    if (trace)
      printTrace();
    return;
  }

  bool redirect = false;
  uint32_t redirectPC = 0;
  EXMEM nExmem = doEX(redirect, redirectPC);
  bool stall = false;
  IDEX nIdex = doID(stall);
  updateFetch(); // may complete a fetch into the buffer this cycle

  // commit: this is the clock edge
  memwb = nMemwb;
  exmem = nExmem;

  if (redirect) {
    // A taken branch/jump in EX. The two younger instructions — the one
    // just decoded and the one fetched (buffered or still in flight) —
    // are on the wrong path. We squash them by committing bubbles
    // instead, and steer the PC
    stats.squashed += (nIdex.valid ? 1 : 0) +
                      ((fBufValid || fOutstanding) ? 1 : 0);
    stats.redirects++;
    idex = IDEX{};
    ifid = IFID{};
    fBufValid = false;
    if (fOutstanding)
      fStale = true; // drain and drop when it lands
    pc = redirectPC;
    char b[80];
    snprintf(b, sizeof b, "taken -> 0x%08x (squashed 2)", redirectPC);
    events.push_back(b);
  } else if (stall) {
    // Load-use interlock: freeze IF and ID (keep ifid, the fetch buffer
    // and pc as they are so ID retries the same instruction next cycle)
    // and inject a bubble into EX. One cycle later MEM/WB->EX forwarding
    // supplies the loaded value and the pipe moves on
    stats.loadUseStalls++;
    idex = IDEX{};
    events.push_back("load-use stall (bubble -> EX)");
  } else {
    // Normal flow: ID takes the fetch buffer's instruction, if any
    idex = nIdex;
    if (fBufValid) {
      ifid = IFID{true, fBufPC, fBufRaw};
      fBufValid = false;
    } else {
      ifid = IFID{};
      stats.fetchStallCycles++;
    }
  }

  msys.tick();
  if (trace)
    printTrace();
}

// Trace: start-of-cycle stage occupancy plus per-cycle event annotations

void CPU::captureStageView() {
  char b[16];
  // IF shows the pc of the instruction occupying the fetch unit: the
  // buffered word, else the fetch in flight, else the next pc to issue
  snprintf(b, sizeof b, "0x%08x", fBufValid ? fBufPC : (fOutstanding ? fPC : pc));
  vIF = b;
  vID = ifid.valid ? disasm(decode(ifid.raw)) : "-";
  vEX = idex.valid ? disasm(idex.ins) : "-";
  vMEM = exmem.valid ? disasm(exmem.ins) : "-";
  vWB = memwb.valid ? disasm(memwb.ins) : "-";
}

void CPU::printTrace() const {
  fprintf(stderr,
          "cyc %6" PRIu64
          " | IF %-10s | ID %-20s | EX %-20s | MEM %-20s | WB %-20s",
          stats.cycles, vIF.c_str(), vID.c_str(), vEX.c_str(), vMEM.c_str(),
          vWB.c_str());
  for (const auto &event : events)
    fprintf(stderr, "  ! %s", event.c_str());
  fputc('\n', stderr);
}
