// Functional unit timing model supporting both pipelined (ALU, branch, AGU,
// pipelined multiplier) and non-pipelined (divider) execution units.

#pragma once

#include <cstdint>
#include <vector>

#include "isa/isa.h"

constexpr uint8_t kNoReg = 0xFF;

// In-flight operation carrying pre-computed execute() outputs through unit latency.
struct FuOp {
  uint64_t seq = 0;
  uint32_t robIdx = 0;
  uint32_t value = 0;
  uint32_t lsqIdx = 0;
  uint32_t target = 0;
  uint8_t destPhysReg = kNoReg;
  bool valid = false;
  bool redirect = false;
};

struct FuUnit {
  const char *name;
  uint32_t latency;
  bool pipelined;

  std::vector<FuOp> stages;
  FuOp currentOp;
  uint32_t remaining = 0;
  FuOp output;

  uint64_t opCount = 0;
  uint64_t busyCycles = 0;

  FuUnit(const char *name, uint32_t latency, bool pipelined)
      : name(name), latency(latency), pipelined(pipelined) {
    if (pipelined)
      stages.resize(latency);
  }

  bool busy() const { return pipelined ? inFlight != 0 : currentOp.valid; }

  bool canAccept() const {
    return pipelined ? !stages[inputSlot].valid
                     : (!currentOp.valid && !output.valid);
  }

  void accept(const FuOp &op) {
    opCount++;

    if (pipelined) {
      stages[inputSlot] = op;
      inFlight++;
    } else {
      currentOp = op;
      remaining = latency;
    }
  }

  // Advances execution by one cycle; returns true when a result enters `output`.
  bool tick() {
    if (!busy())
      return false;

    busyCycles++;

    if (pipelined) {
      // Stall pipeline advance while writeback has not consumed `output`.
      if (output.valid)
        return false;

      // Rotating ring index avoids shifting the stages vector every cycle.
      inputSlot = inputSlot ? inputSlot - 1 : latency - 1;
      FuOp &exiting = stages[inputSlot];

      if (exiting.valid) {
        output = exiting;
        exiting.valid = false;
        inFlight--;
        return true;
      }
    } else if (--remaining == 0) {
      output = currentOp;
      currentOp.valid = false;
      return true;
    }

    return false;
  }

  // Squashes every in-flight or output op younger than `seq` on recovery.
  void flushYounger(uint64_t seq) {
    for (FuOp &stage : stages) {
      if (stage.valid && stage.seq > seq) {
        stage.valid = false;
        inFlight--;
      }
    }

    if (currentOp.valid && currentOp.seq > seq) {
      currentOp.valid = false;
      remaining = 0;
    }

    if (output.valid && output.seq > seq)
      output.valid = false;
  }

private:
  uint32_t inputSlot = 0;
  uint32_t inFlight = 0;
};
