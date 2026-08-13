// ==============================================================
// The classic 5-stage in-order RISC-V pipeline
// ==============================================================
//
//                +----+   +----+   +----+   +-----+   +----+
//   instruction  | IF |-->| ID |-->| EX |-->| MEM |-->| WB |
//   flow ------> +----+   +----+   +----+   +-----+   +----+
//                 fetch    decode   ALU /    data      reg-
//                          + reg    branch   memory    file
//                          read     resolve  access    write
//
// Microarchitecture
//      - In-order, single-issue, one instruction per stage per cycle
//      - Pipeline registers (latches) between stages: IF/ID, ID/EX,
//        EX/MEM, MEM/WB. Each latch carries a "valid" bit; an invalid
//        latch is a bubble
//      - ALU forwarding: EX/MEM -> EX and MEM/WB -> EX, newest value
//        wins
//      - Store-data forwarding: MEM/WB -> MEM, which makes the common
//        "lw x1 ... ; sw x1 ..." pattern run without any stall
//      - One-cycle load-use interlock: a load followed immediately
//        by an instruction that needs the loaded value in EX stalls
//        for one cycle
//      - Control transfers (branches, jal, jalr) are resolved in EX
//        with a static predict-not-taken policy. A taken transfer
//        squashes the two younger instructions already in IF and ID,
//        incurring a 2-cycle penalty
//      - The register file is "write-first" meaning a WB writes in the
//        first half of the cycle, ID reads in the second half. We model
//        this by simply running the WB stage before the ID stage each
//        cycle
//
// Memory interface (phase 1)
//      IF and MEM no longer read memory directly: each talks to a
//      MemPort (an L1 cache, or flat memory in --flat mode) that may
//      take several cycles to answer.
//      - IF is a small fetch unit: it issues a fetch for pc, holds the
//        completed word in a one-entry buffer until ID can take it, and
//        marks an in-flight fetch stale on a redirect (the response is
//        drained and dropped). With a 1-cycle port this collapses to
//        exactly the phase-0 fetch timing.
//      - MEM issues its load/store when the instruction first enters
//        the stage (capturing store-data forwarding at that instant)
//        and holds the whole pipeline behind it until the response
//        arrives; WB receives bubbles meanwhile. Because stages only
//        advance together, all phase-0 forwarding invariants are
//        preserved during and after a stall.
//
// Simulation technique
//      On every cycle, we compute the next value of every pipeline latch
//      from the current values, then commit them all at once. This
//      double-buffered update models the simultaneous clock edge of real
//      hardware and makes stage evaluation order irrelevant for correctness
//      except the one place we indicate.
//
// Limitations
//      - No CSRs, interrupts, exceptions, privilege levels. Misaligned
//        or out-of-bounds accesses terminate the simulation with a message.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/commit.h"
#include "isa/isa.h"
#include "memory/system.h"
#include "sim/config.h"
#include "sim/stats.h"

// Pipeline latches (registers)
//
// Each struct is the register file between two stages, named after the
// stages it separates. valid == false means the slot holds a bubble, because
// either the pipe hasn't filled yet, or a stall/flush inserted one.
// Default-constructing a latch yields a bubble, which we exploit when
// squashing: e.g., idex = IDEX{}

struct IFID {
  bool valid = false;
  uint32_t pc = 0; // address the word was fetched from
  uint32_t raw = 0;
};

struct IDEX {
  bool valid = false;
  uint32_t pc = 0;
  Instr ins; // the decoded instruction
  // EX may override these with forwarded values
  uint32_t rs1val = 0;
  uint32_t rs2val = 0;
};

struct EXMEM {
  bool valid = false;
  uint32_t pc = 0;
  Instr ins;
  uint32_t aluResult = 0; // ALU result / effective address / link address
  uint32_t storeData = 0; // rs2 value for stores (already EX-forwarded)
};

struct MEMWB {
  bool valid = false;
  uint32_t pc = 0;
  Instr ins;
  uint32_t result = 0; // value to write into rd: load data or ALU result
  // The store this instruction performed in MEM, if any. Memory is
  // written in MEM (one stage before retirement), and the value written
  // (after store-data forwarding) only exists there — so MEM records it
  // here for WB to put in the instruction's CommitRecord
  std::optional<MemoryWrite> memWrite;
};

// The CPU

class CPU {
public:
  CPU(const SimConfig &cfg, MemorySystem &msys);

  // Load a program image (vector of 32-bit words) at address 0
  void loadWords(const std::vector<uint32_t> &words);
  void loadBytes(const std::vector<uint8_t> &bytes);

  // Run until an exit syscall / ebreak / error, or the cycle budget runs out
  int run();

  void dumpRegs() const;

  // Called once per retired instruction with its CommitRecord, in
  // retirement order. Null by default; a differential checker plugs in
  // here to compare the pipeline against the functional reference model
  std::function<void(const CommitRecord &)> onCommit;

private:
  // memory system
  MemorySystem &msys;
  MemPort *imem; // fetch port (L1I or flat)
  MemPort *dmem; // data port (L1D or flat)
  bool showMemStats;

  // architectural state
  uint32_t regs[32];
  uint32_t pc = 0; // next address the fetch unit will request

  // pipeline state
  IFID ifid;
  IDEX idex;
  EXMEM exmem;
  MEMWB memwb;

  // fetch unit state (the IF stage)
  bool fOutstanding = false; // a fetch is in flight at imem
  bool fStale = false;       // drop the in-flight fetch when it lands
  uint32_t fPC = 0;          // pc of the in-flight fetch
  bool fBufValid = false;    // one-entry buffer of a completed fetch
  uint32_t fBufPC = 0;
  uint32_t fBufRaw = 0;

  // data access state (the MEM stage)
  bool dOutstanding = false; // a load/store is in flight at dmem
  uint32_t dAddr = 0;        // saved at issue for the commit record
  uint32_t dVal = 0;
  uint8_t dSize = 0;

  // bookkeeping
  bool halted = false;
  int exitCode = 0;
  bool trace;
  uint64_t maxCycles;
  Stats stats;
  std::vector<std::string> events; // per-cycle annotations for the trace

  // pipeline stages
  void updateFetch(); // run the fetch unit for one cycle
  IDEX doID(bool &stall);
  uint32_t fwd(uint8_t reg, uint32_t valueFromID) const;
  EXMEM doEX(bool &redirect, uint32_t &redirectPC);
  void checkAlign(Op op, uint32_t addr, uint32_t atPc);
  MEMWB doMEM(bool &memDone);
  void doWB();
  void doSyscall();
  void stepCycle();

  // trace
  std::string vIF, vID, vEX, vMEM, vWB; // start-of-cycle stage occupancy
  void captureStageView();
  void printTrace() const;
};
