// The reorder buffer.
//
// Allocated at dispatch in program order; results mark entries done
// out of order; commit consumes only from the head, up to commit-width
// per cycle. Architectural state changes nowhere else: the rename
// map's displaced mapping is freed here, stores are released to the
// store buffer here, syscalls and traps take effect here; the
// machine is therefore precise at every instruction boundary.
//
// Recovery walks the tail: entries younger than a mispredicted branch
// are popped one by one, from the youngest back toward the branch,
// each returning its physical register and restoring the mapping it
// displaced.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"

struct RobEntry {
  uint64_t seq = 0;
  uint32_t pc = 0;
  Instr ins;
  bool done = false;
  uint8_t pdst = 0xFF;     // physical dest (kNoReg if none)
  uint8_t prevPhys = 0xFF; // mapping displaced at rename; freed at commit
  bool isMem = false;      // has an LSQ entry
  bool isSystem = false;   // fence/ecall/ebreak/illegal: effect at commit
  // Faults detected speculatively take effect only if the instruction
  // commits; a garbage address from the wrong path must not kill
  // the run
  uint8_t fault = 0; // 0 none, 1 misaligned, 2 out of bounds
  // Branch bookkeeping: what fetch predicted, what execute resolved,
  // and the predictor snapshot (counter index, pre-shift history) so
  // training touches the counter the prediction consulted and a
  // mispredict can rewind the speculative history
  bool isBranch = false;
  bool predictedTaken = false, actualTaken = false;
  uint32_t predictedTarget = 0, actualTarget = 0;
  uint32_t predIdx = 0, ghrBefore = 0;
  // Syscall argument registers captured at dispatch (the map is
  // architectural there because the ROB was empty), read at commit
  uint8_t sysA0 = 0, sysA7 = 0;
};

class ROB {
public:
  explicit ROB(uint32_t size) : e(size) {}

  bool empty() const { return n == 0; }
  bool full() const { return n == e.size(); }
  uint32_t count() const { return n; }
  uint32_t size() const { return (uint32_t)e.size(); }

  uint32_t alloc() { // returns the entry index; caller fills it
    uint32_t idx = (headIdx + n) % (uint32_t)e.size();
    e[idx] = RobEntry{};
    n++;
    return idx;
  }

  RobEntry &at(uint32_t idx) { return e[idx]; }
  RobEntry &head() { return e[headIdx]; }
  RobEntry &tail() { return e[(headIdx + n - 1) % (uint32_t)e.size()]; }

  void popHead() {
    headIdx = (headIdx + 1) % (uint32_t)e.size();
    n--;
  }
  void popTail() { n--; }

private:
  std::vector<RobEntry> e;
  uint32_t headIdx = 0;
  uint32_t n = 0;
};
