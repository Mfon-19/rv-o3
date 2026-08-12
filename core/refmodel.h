// The functional reference model: the executable form of the ISA spec.
//
// A plain fetch-decode-execute-commit interpreter. One instruction at a
// time, in program order, each fully completed before the next begins.
// No pipeline, no latches, no forwarding, no stalls — nothing here can
// be subtly wrong about *when*, because there is no when. It answers
// only what each instruction does to architectural state.
//
// It shares decode() and execute() with every timing model, so the two
// can only diverge in what a timing model adds on top: operand routing,
// hazard handling, speculation, recovery. That is exactly what
// comparing their commit streams (core/commit.h) is meant to check.
//
// The model mirrors the timing models' non-ISA policies exactly:
// the same syscall interface, and the same fatal-error behavior on
// misaligned or out-of-bounds accesses.

#pragma once

#include <cstdint>
#include <vector>

#include "core/commit.h"
#include "isa/isa.h"
#include "memory/memory.h"
#include "sim/config.h"

class RefModel {
public:
  explicit RefModel(const SimConfig &cfg);

  Memory mem;

  // Suppress all output (syscall prints and halt messages) while still
  // honoring exit. Set when the model runs as a silent shadow of a
  // timing model in a differential check, so side effects happen once
  bool quiet = false;

  // Load a program image at address 0
  void loadWords(const std::vector<uint32_t> &words);
  void loadBytes(const std::vector<uint8_t> &bytes);

  // Execute one instruction and optionally describe it in *rec.
  // Returns true if an instruction was executed, false if the model
  // had already halted.
  bool step(CommitRecord *rec = nullptr);

  // Run until an exit syscall / ebreak / error, or the instruction
  // budget runs out; returns the exit code
  int run();

  void dumpRegs() const;

  bool halted() const { return halted_; }
  int exitCode() const { return exitCode_; }
  uint64_t retired() const { return retired_; }

private:
  // architectural state — this is all of it
  uint32_t regs[32];
  uint32_t pc = 0;

  bool halted_ = false;
  int exitCode_ = 0;
  uint64_t retired_ = 0;
  uint64_t maxInstrs; // reuses the -c budget, counted in instructions

  void checkAlign(Op op, uint32_t addr) const;
  void doSyscall();
};
