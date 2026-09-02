// The architectural commit stream.
//
// One CommitRecord describes what an instruction did to architectural
// state when it retired: the register it wrote, the memory it wrote,
// any exception it raised. A store is recorded when commit hands it to
// the store buffer, before the cache write lands; a fatal fault
// (misaligned or out-of-bounds access) ends the run before any record
// is emitted. The reference model and the timing core both emit this
// record, so the two can be compared instruction by instruction: the
// first mismatch pinpoints where the core corrupted architectural
// state.

#pragma once

#include <cstdint>
#include <optional>

struct RegisterWrite {
  uint8_t rd = 0;
  uint32_t value = 0;
};
inline bool operator==(const RegisterWrite &a, const RegisterWrite &b) {
  return a.rd == b.rd && a.value == b.value;
}

struct MemoryWrite {
  uint32_t addr = 0;
  uint32_t value = 0; // truncated to size
  uint8_t size = 0;   // 1, 2, or 4 bytes

  MemoryWrite() = default;
  MemoryWrite(uint32_t addr, uint32_t value, uint8_t size)
      : addr(addr),
        value(size == 4 ? value : value & ((1u << (8 * size)) - 1)),
        size(size) {}
};
inline bool operator==(const MemoryWrite &a, const MemoryWrite &b) {
  return a.addr == b.addr && a.value == b.value && a.size == b.size;
}

enum class ExceptionKind : uint8_t {
  IllegalInstruction,
};

struct Exception {
  ExceptionKind kind = ExceptionKind::IllegalInstruction;
};
inline bool operator==(const Exception &a, const Exception &b) {
  return a.kind == b.kind;
}

struct CommitRecord {
  uint64_t sequence = 0;    // 0-based retirement index
  uint32_t pc = 0;          // address the instruction was fetched from
  uint32_t instruction = 0; // raw encoding
  std::optional<RegisterWrite> registerWrite;
  std::optional<MemoryWrite> memoryWrite;
  std::optional<Exception> exception;
};
inline bool operator==(const CommitRecord &a, const CommitRecord &b) {
  return a.sequence == b.sequence && a.pc == b.pc &&
         a.instruction == b.instruction && a.registerWrite == b.registerWrite &&
         a.memoryWrite == b.memoryWrite && a.exception == b.exception;
}
