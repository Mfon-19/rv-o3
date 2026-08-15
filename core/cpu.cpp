#include "core/cpu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "isa/execute.h"

CPU::CPU(const SimConfig &cfg, MemorySystem &msys)
    : msys(msys), imem(msys.imem),
      showMemStats(!cfg.flatMemory || cfg.flatLatency > 1),
      rob(cfg.retireQueueSize), alus(cfg.aluCount, FuUnit("alu", 1, true)),
      brUnit("br", 1, true), mulUnit("mul", cfg.mulLatency, cfg.mulPipelined),
      divUnit("div", cfg.divLatency, false), trace(cfg.trace),
      maxCycles(cfg.maxCycles), wbPorts(cfg.wbPorts) {
  memset(regs, 0, sizeof regs);
  lsu.dmem = msys.dmem;
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
  stats.dataStallCycles = lsu.dataStallCycles;
  stats.printCore();
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
  fuLine("lsu", lsu.busyCycles, lsu.ops);
  fputc('\n', stderr);
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

// The fetch unit (IF stage). Runs every cycle: it drains a completed
// fetch into the one-entry buffer (dropping it if a redirect made it
// stale) and issues a fetch for pc as soon as it has nothing in flight
// and nothing buffered. pc advances at issue; a redirect rewrites it
// and empties the buffer
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

// Retire: consume done entries from the retire-queue head, in program
// order. Each retirement emits the instruction's CommitRecord; system
// ops take their architectural effect here, observing fully settled
// state (their issue-time serialization guarantees nothing is in
// flight around them)
void CPU::retire() {
  int n = 0;
  while (!rob.empty() && rob.head().done && !halted) {
    RetireEntry &e = rob.head();
    const Instr &I = e.ins;

    CommitRecord rec;
    rec.sequence = stats.retired;
    rec.pc = e.pc;
    rec.instruction = I.raw;
    rec.memoryWrite = e.memWrite;

    switch (I.op) {
    case Op::ECALL:
      stats.retired++;
      doSyscall(e.pc);
      break;
    case Op::EBREAK:
      stats.retired++;
      fprintf(stderr, "ebreak at pc=0x%08x — halting\n", e.pc);
      halted = true;
      break;
    case Op::ILLEGAL:
      // True-path illegal words trap here; wrong-path garbage after a
      // taken branch is squashed in the frontend and never issues
      fprintf(stderr, "illegal instruction 0x%08x at pc=0x%08x\n", I.raw,
              e.pc);
      halted = true;
      exitCode = 1;
      rec.exception = Exception{ExceptionKind::IllegalInstruction};
      break;
    default:
      if (e.hasRegWrite)
        rec.registerWrite = RegisterWrite{I.rd, e.value};
      stats.retired++;
      break;
    }

    if (n++ == 0)
      vRT = disasm(I);
    if (onCommit)
      onCommit(rec);
    rob.pop();
  }
  if (n > 1)
    vRT += " +" + std::to_string(n - 1);
}

// Writeback: units that finished hold their result in their output
// slot; the ports write back up to wbPorts of them per cycle,
// oldest-first (the issue sequence number is the age). Winners write
// the register file, clear the scoreboard, broadcast to any store
// still waiting for that value, and mark their retire-queue entry
// done. Losers keep their output slot, which backpressures the unit
void CPU::writeback() {
  std::vector<FuOp *> cands;
  for (FuUnit &u : alus)
    if (u.out.valid)
      cands.push_back(&u.out);
  for (FuUnit *u : {&brUnit, &mulUnit, &divUnit})
    if (u->out.valid)
      cands.push_back(&u->out);
  if (lsu.out.valid)
    cands.push_back(&lsu.out);
  std::sort(cands.begin(), cands.end(),
            [](const FuOp *a, const FuOp *b) { return a->seq < b->seq; });

  size_t winners = std::min((size_t)wbPorts, cands.size());
  for (size_t i = 0; i < winners; i++) {
    FuOp &op = *cands[i];
    const Instr &I = op.ins;
    RetireEntry &e = rob.at(op.robIdx);
    if (writesRd(I.op) && I.rd != 0) {
      regs[I.rd] = op.value;
      sb.clear(I.rd); // WAW blocking: this op was the only writer
      lsu.capture(I.rd, op.value);
      e.hasRegWrite = true;
      e.value = op.value;
    }
    e.memWrite = op.memWrite;
    e.done = true;
    if (i == 0)
      vWB = disasm(I);
    op = FuOp{}; // free the output slot
  }
  if (cands.size() > winners) {
    stats.wbConflicts += cands.size() - winners;
    events.push_back("wb port conflict");
  }
}

// Issue: the one instruction per cycle that may enter a functional
// unit, in program order. Operands come straight from the register
// file — writeback already ran this cycle, so anything not pending in
// the scoreboard is fresh
void CPU::issue() {
  if (!isl.valid) {
    stats.fetchStallCycles++; // frontend had nothing for us
    return;
  }
  const Instr &I = isl.ins;

  // System ops serialize: wait for the machine to drain, then take
  // effect at retirement. An empty retire queue means nothing is in
  // flight anywhere
  if (fuKindOf(I.op) == FuKind::NONE) {
    if (!rob.empty()) {
      stats.serializeStalls++;
      events.push_back("serialize wait");
      return;
    }
    rob.at(rob.alloc(isl.pc, I)).done = true;
    isl.valid = false;
    return;
  }

  // RAW: a source operand is still being produced. Stores only need
  // rs1 (the address) now; the data may arrive via the writeback
  // broadcast up until the access starts
  if ((usesRs1(I.op) && sb.busy(I.rs1)) ||
      (rs2NeededAtIssue(I.op) && sb.busy(I.rs2))) {
    stats.rawStalls++;
    events.push_back("raw stall");
    return;
  }
  // WAW: the destination is still being produced by an older
  // instruction; completing out of order would misorder the writes
  if (writesRd(I.op) && I.rd != 0 && sb.busy(I.rd)) {
    stats.wawStalls++;
    events.push_back("waw stall");
    return;
  }
  if (rob.full()) {
    stats.robFullStalls++;
    events.push_back("rob full");
    return;
  }

  // Structural: is the functional unit free?
  FuUnit *unit = nullptr;
  const FuKind kind = fuKindOf(I.op);
  switch (kind) {
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
  default:
    break; // LSU, handled below
  }
  if (kind == FuKind::LSU ? !lsu.canAccept() : unit == nullptr) {
    stats.structStalls++;
    events.push_back(kind == FuKind::LSU ? "lsu busy"
                     : kind == FuKind::DIV
                         ? "div busy"
                         : kind == FuKind::MUL ? "mul busy" : "fu busy");
    return;
  }

  // Go: read operands, compute the result now (semantics are pure),
  // and hand it to the unit to delay its visibility
  const uint32_t a = usesRs1(I.op) ? regs[I.rs1] : 0;
  const uint32_t b = usesRs2(I.op) ? regs[I.rs2] : 0;
  const ExecResult r = execute(I, isl.pc, a, b);

  FuOp op;
  op.valid = true;
  op.seq = issueSeq++;
  op.robIdx = rob.alloc(isl.pc, I);
  op.ins = I;
  op.pc = isl.pc;
  op.value = r.value;
  if (isStore(I.op)) {
    op.storeData = b;
    op.storeDataReg = I.rs2;
    op.storeDataReady = !sb.busy(I.rs2);
  }
  if (kind == FuKind::LSU)
    lsu.accept(op);
  else
    unit->accept(op);
  if (writesRd(I.op))
    sb.set(I.rd);

  if (r.redirect) {
    // Resolved taken branch/jump. The squash and pc change are applied
    // at the clock edge, after the frontend has run on the old pc —
    // same 2-cycle penalty as resolving in a dedicated stage
    pendRedirect = true;
    pendTarget = r.target;
  }
  isl.valid = false;
}

void CPU::tickUnits() {
  for (FuUnit &u : alus)
    u.tick();
  brUnit.tick();
  mulUnit.tick();
  divUnit.tick();
}

// Minimal syscall interface (a7 = number, a0 = argument)
//   1  print a0 as a signed decimal integer, followed by a newline
//   2  print a0 as a single ASCII character
//   3  print the NUL-terminated string at address a0
//   93 exit with code a0  (Linux-flavoured number; 10 also accepted)
void CPU::doSyscall(uint32_t atPc) {
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
            atPc);
    break;
  }
}

// One clock cycle.
//
// Stages run in reverse pipeline order, so each consumes what the
// stage behind it produced in an earlier cycle. The deliberate
// same-cycle orderings:
//      1. retire before writeback: an entry marked done this cycle
//         retires next cycle, keeping the commit point one stage
//         past writeback
//      2. writeback before issue: values written back this cycle are
//         readable at issue this cycle (this replaces the forwarding
//         network)
//      3. writeback before the LSU: a store waiting on its data
//         captures the broadcast before its access starts
//      4. the frontend runs on the pre-redirect pc; the redirect is
//         applied at the clock edge like every other state change
void CPU::stepCycle() {
  stats.cycles++;
  events.clear();
  vWB = "-";
  vRT = "-";
  if (trace)
    captureStageView();

  retire();
  if (halted) {
    if (trace)
      printTrace();
    return;
  }
  writeback();
  lsu.operate(&events);
  issue();

  // ID: decode into the issue slot the moment it frees up
  if (!isl.valid && ifid.valid) {
    isl.valid = true;
    isl.pc = ifid.pc;
    isl.ins = decode(ifid.raw);
    ifid.valid = false;
  }
  // IF: the fetch unit runs, then hands its buffer to ID's latch
  updateFetch();
  if (!ifid.valid && fBufValid) {
    ifid = IFID{true, fBufPC, fBufRaw};
    fBufValid = false;
  }

  if (pendRedirect) {
    // A taken branch/jump resolved at issue. Everything younger is
    // still in the frontend — squash it there and steer the pc
    uint32_t k = (isl.valid ? 1 : 0) + (ifid.valid ? 1 : 0) +
                 ((fBufValid || fOutstanding) ? 1 : 0);
    stats.squashed += k;
    stats.redirects++;
    isl.valid = false;
    ifid = IFID{};
    fBufValid = false;
    if (fOutstanding)
      fStale = true; // drain and drop when it lands
    pc = pendTarget;
    pendRedirect = false;
    char b[80];
    snprintf(b, sizeof b, "taken -> 0x%08x (squashed %u)", pendTarget, k);
    events.push_back(b);
  }

  tickUnits();
  msys.tick();
  if (trace)
    printTrace();
}

// Trace: start-of-cycle frontend occupancy, the writeback winner and
// retirements as they happen, plus per-cycle event annotations

void CPU::captureStageView() {
  char b[16];
  // IF shows the pc of the instruction occupying the fetch unit: the
  // buffered word, else the fetch in flight, else the next pc to issue
  snprintf(b, sizeof b, "0x%08x",
           fBufValid ? fBufPC : (fOutstanding ? fPC : pc));
  vIF = b;
  vID = ifid.valid ? disasm(decode(ifid.raw)) : "-";
  vIS = isl.valid ? disasm(isl.ins) : "-";
}

void CPU::printTrace() const {
  fprintf(stderr,
          "cyc %6" PRIu64
          " | IF %-10s | ID %-20s | IS %-20s | WB %-20s | RT %-20s",
          stats.cycles, vIF.c_str(), vID.c_str(), vIS.c_str(), vWB.c_str(),
          vRT.c_str());
  for (const auto &event : events)
    fprintf(stderr, "  ! %s", event.c_str());
  fputc('\n', stderr);
}
