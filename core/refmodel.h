// The differential-checking reference model: the executable form of the ISA spec.
// Used only to verify instructions retired by the out-of-order core.
//
// A plain fetch-decode-execute-commit interpreter. One instruction at a
// time, in program order, each fully completed before the next begins.
// No pipeline, no forwarding, no speculation, no stalls; nothing here
// can be wrong about *when*, because there is no when. It answers only
// what each instruction does to architectural state.
//
// It shares decode() and execute() with the timing core, so it is not
// an independent oracle for those; the two can only diverge in what
// the core adds on top: operand routing, hazard handling, memory
// ordering, speculation, recovery, retirement. That is exactly what
// comparing their commit streams (core/commit.h) checks. The bench/
// host builds cover the ISA semantics from outside.
//
// The non-ISA policies are shared outright rather than mirrored: the
// syscall interface (sim/syscall.h) and the fatal-access checks
// (memory/memory.h) are the same code in both models.

#pragma once

#include <cstdint>

#include "core/commit.h"
#include "isa/isa.h"
#include "memory/memory.h"
#include "sim/config.h"

class RefModel {
public:
  explicit RefModel(const SimConfig &cfg);

  Memory mem;

  // A differential shadow consumes the primary core's recorded byte/key event.
  uint32_t replayInput = UINT32_MAX;

  // Execute one instruction and optionally describe it in *rec.
  // Returns false if the model had already halted
  bool step(CommitRecord *rec = nullptr);

  bool halted() const { return halted_; }
  int exitCode() const { return exitCode_; }
  uint64_t retired() const { return retired_; }
  uint32_t reg(int i) const { return regs[i]; }

private:
  // The architectural state, apart from mem above
  uint32_t regs[32];
  uint32_t pc = 0;

  bool halted_ = false;
  int exitCode_ = 0;
  uint64_t retired_ = 0;
};
