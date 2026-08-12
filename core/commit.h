// The architectural commit stream.
//
// One CommitRecord describes everything an instruction did to
// architectural state when it retired: which register it wrote, what
// memory it wrote, and any exception it raised. Every model that
// executes instructions — the functional reference model and each
// timing model — emits the same record type, so two models can be
// compared instruction by instruction: the first mismatching record
// pinpoints exactly where a timing model corrupted architectural state.

#pragma once

#include <cstdint>
#include <optional>

struct RegisterWrite {
  uint8_t rd = 0;
  uint32_t value = 0;
};

struct MemoryWrite {
  uint32_t addr = 0;
  uint32_t value = 0; // truncated to size
  uint8_t size = 0;   // 1, 2, or 4 bytes
};

enum class ExceptionKind : uint8_t {
  IllegalInstruction,
};

struct Exception {
  ExceptionKind kind = ExceptionKind::IllegalInstruction;
};

struct CommitRecord {
  uint64_t sequence = 0;    // 0-based retirement index
  uint32_t pc = 0;          // address the instruction was fetched from
  uint32_t instruction = 0; // raw encoding
  std::optional<RegisterWrite> registerWrite;
  std::optional<MemoryWrite> memoryWrite;
  std::optional<Exception> exception;
};
