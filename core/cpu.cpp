#include "core/cpu.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isa/execute.h"

CPU::CPU(const SimConfig &cfg)
    : mem(cfg.memBytes), trace(cfg.trace), maxCycles(cfg.maxCycles) {
  memset(regs, 0, sizeof regs);
}

void CPU::loadWords(const std::vector<uint32_t> &words) {
  for (size_t i = 0; i < words.size(); i++) {
    mem.store32((uint32_t)(i * 4), words[i]);
  }
}

void CPU::loadBytes(const std::vector<uint8_t> &bytes) {
  mem.check(0, (uint32_t)bytes.size(), "program image");
  memcpy(mem.bytes.data(), bytes.data(), bytes.size());
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
  stats.print(exitCode);
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

// IF - instruction fetch stage
IFID CPU::doIF() {
  if (pc % 4 != 0) {
    fprintf(stderr, "fatal: misaligned fetch at pc=0x%8x\n", pc);
    exit(1);
  }
  IFID out;
  out.valid = true;
  out.pc = pc;
  out.raw = mem.load32(pc);
  return out;
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

// MEM - data memory stage
MEMWB CPU::doMEM() {
  MEMWB out;
  if (!exmem.valid)
    return out;
  const Instr &I = exmem.ins;

  out.valid = true;
  out.pc = exmem.pc;
  out.ins = I;
  out.result = exmem.aluResult;

  const uint32_t addr = exmem.aluResult;

  if (isLoad(I.op)) {
    checkAlign(I.op, addr, exmem.pc);
    switch (I.op) {
    // Sub-word loads sign- or zero-extended into 32 bits as per the ISA
    case Op::LB:
      out.result = (uint32_t)(int32_t)(int8_t)mem.load8(addr);
      break;
    case Op::LBU:
      out.result = mem.load8(addr);
      break;
    case Op::LH:
      out.result = (uint32_t)(int32_t)(int16_t)mem.load16(addr);
      break;
    case Op::LHU:
      out.result = mem.load16(addr);
      break;
    case Op::LW:
      out.result = mem.load32(addr);
      break;
    default:
      break;
    }
  } else if (isStore(I.op)) {
    checkAlign(I.op, addr, exmem.pc);
    uint32_t data = exmem.storeData;
    // MEM/WB -> MEM store-data forwarding. Covers the case "lw x1 / sw x1":
    // when the store sat in EX, the load's data did not exist yet, so the
    // value carried in the storeData is stale, now the load is in WB and its
    // data is in the MEM/WB latch. The instruction in WB is always the store's
    // immediate predecessor, so its value is never outdated
    if (memwb.valid && writesRd(memwb.ins.op) && memwb.ins.rd != 0 &&
        memwb.ins.rd == I.rs2) {
      data = memwb.result;
    }
    switch (I.op) {
    case Op::SB:
      mem.store8(addr, (uint8_t)data);
      break;
    case Op::SH:
      mem.store16(addr, (uint16_t)data);
      break;
    case Op::SW:
      mem.store32(addr, data);
      break;
    default:
      break;
    }
  }
  return out;
}

// WB - write-back stage
void CPU::doWB() {
  if (!memwb.valid)
    return;
  const Instr &I = memwb.ins;

  switch (I.op) {
  case Op::ECALL:
    stats.retired++;
    doSyscall();
    return;
  case Op::EBREAK:
    stats.retired++;
    fprintf(stderr, "ebreak at pc=0x%08x — halting\n", memwb.pc);
    halted = true;
    return;
  case Op::ILLEGAL:
    // We let illegal words flow down the pipe and trap them only here,
    // so that wrong-path garbage fetched after a taken branch (which
    // gets squashed long before WB) never causes a spurious error.
    fprintf(stderr, "illegal instruction 0x%08x at pc=0x%08x\n", I.raw,
            memwb.pc);
    halted = true;
    exitCode = 1;
    return;
  default:
    break;
  }

  // The one and only architectural register write
  if (writesRd(I.op) && I.rd != 0)
    regs[I.rd] = memwb.result;
  stats.retired++;
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
    for (uint32_t a = arg; mem.load8(a) != 0; a++)
      putchar(mem.load8(a));
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
// Evaluation order matters for these two intra-cycle dependencies we model:
//      1. WB writes the register file BEFORE ID reads it
//      2. MEM reads the current MEM/WB latch for store-data forwarding,
//         and EX reads the current EX/MEM and MEM/WB latches for ALU
//         forwarding, hence, all stages compute their output into local
//         "next" latches which are committed together at the end
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
  MEMWB nMemwb = doMEM();
  bool redirect = false;
  uint32_t redirectPC = 0;
  EXMEM nExmem = doEX(redirect, redirectPC);
  bool stall = false;
  IDEX nIdex = doID(stall);
  IFID nIfid = doIF();

  // commit: this is the clock edge
  memwb = nMemwb;
  exmem = nExmem;

  if (redirect) {
    // A taken branch/jump in EX. The two younger instructions, the one
    // just decoded and the one just fetched, are on the wrong path. We
    // squash them by committing bubbles instead, and steer the PC
    stats.squashed += (nIdex.valid ? 1 : 0) + (nIfid.valid ? 1 : 0);
    stats.redirects++;
    idex = IDEX{};
    ifid = IFID{};
    pc = redirectPC;
    char b[80];
    snprintf(b, sizeof b, "taken -> 0x%08x (squashed 2)", redirectPC);
    events.push_back(b);
  } else if (stall) {
    // Load-use interlock: freeze IF and ID (keep ifid and pc as they
    // are so ID retries the same instruction next cycle) and inject a
    // bubble into EX. One cycle later MEM/WB->EX forwarding supplies
    // the loaded value and the pipe moves on
    stats.loadUseStalls++;
    idex = IDEX{};
    events.push_back("load-use stall (bubble -> EX)");
  } else {
    // Normal flow
    idex = nIdex;
    ifid = nIfid;
    pc += 4;
  }

  if (trace)
    printTrace();
}

// Trace: start-of-cycle stage occupancy plus per-cycle event annotations

void CPU::captureStageView() {
  char b[16];
  snprintf(b, sizeof b, "0x%08x", pc);
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
