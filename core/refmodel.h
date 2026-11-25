// Architectural commit record shared by OoOCore and RefModel, plus the
// timing-free functional RV32IM reference interpreter used by `-d`.

#pragma once

#include <cstdint>
#include <optional>

#include "isa/isa.h"
#include "memory/memory.h"
#include "sim/config.h"

struct RegisterWrite {
  uint8_t rd = 0;
  uint32_t value = 0;
  bool operator==(const RegisterWrite &) const = default;
};

struct MemoryWrite {
  uint32_t addr = 0;
  uint32_t value = 0;
  uint8_t size = 0;

  MemoryWrite() = default;
  MemoryWrite(uint32_t addr, uint32_t value, uint8_t size)
      : addr(addr),
        value(size == 4 ? value : value & ((1u << (8 * size)) - 1)),
        size(size) {}

  bool operator==(const MemoryWrite &) const = default;
};

enum class ExceptionKind : uint8_t { IllegalInstruction };

struct Exception {
  ExceptionKind kind = ExceptionKind::IllegalInstruction;
  bool operator==(const Exception &) const = default;
};

// Exact architectural side effects of one retired instruction.
struct CommitRecord {
  uint64_t sequence = 0;
  uint32_t pc = 0;
  uint32_t instruction = 0;
  std::optional<RegisterWrite> registerWrite;
  std::optional<MemoryWrite> memoryWrite;
  std::optional<Exception> exception;

  bool operator==(const CommitRecord &) const = default;
};

// Timing-free RV32IM interpreter stepped in lockstep with OoOCore under `-d`.
class RefModel {
public:
  explicit RefModel(const SimConfig &config);

  Memory mem;

  // Input value captured by OoOCore for non-deterministic syscalls (5 and 7).
  uint32_t replayInput = UINT32_MAX;

  bool step(CommitRecord *outRecord = nullptr);

  bool halted() const { return halted_; }
  int exitCode() const { return exitCode_; }
  uint64_t retired() const { return retired_; }
  uint32_t reg(int idx) const { return regs[idx]; }

private:
  uint32_t regs[32];
  uint32_t pc = 0;
  bool halted_ = false;
  int exitCode_ = 0;
  uint64_t retired_ = 0;
};
