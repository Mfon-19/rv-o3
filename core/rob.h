// The reorder buffer.
//
// Allocated at dispatch in program order; results mark entries done
// out of order; commit consumes only from the head, up to width per
// cycle. State becomes architectural nowhere else: the displaced rename
// mapping is freed here, stores are released to the store buffer here,
// syscalls and traps take effect here. (The cache write for a committed
// store lands later, but the store buffer keeps its order and forwards
// its value to younger loads meanwhile.) Faults wait for the head, so
// the machine is precise at every instruction boundary.
//
// Recovery walks the tail: entries younger than the mispredicted branch
// (or the replaying load) pop one by one, from the youngest back, each
// returning its physical register and restoring the mapping it
// displaced.

#pragma once

#include <cstdint>

#include "core/ring.h"
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
  // A fault detected speculatively takes effect only if the instruction
  // commits; a garbage address from the wrong path must not kill the run
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

  // Valid once the branch has resolved: did fetch guess wrong?
  bool mispredicted() const {
    return actualTaken != predictedTaken ||
           (actualTaken && actualTarget != predictedTarget);
  }
};

using ROB = Ring<RobEntry>;
