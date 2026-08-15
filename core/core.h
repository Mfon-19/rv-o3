// The interface every core model implements.
//
// The simulator hosts more than one timing model of the same
// architecture (the scoreboarded in-order core, the out-of-order
// core). The driver, the loader, and the differential checker only
// need this much: load a program, run it, report architectural
// registers, and hand every retired instruction's CommitRecord to
// whoever is listening.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/commit.h"

class Core {
public:
  virtual ~Core() = default;

  // Load a program image at address 0
  virtual void loadWords(const std::vector<uint32_t> &words) = 0;
  virtual void loadBytes(const std::vector<uint8_t> &bytes) = 0;

  // Run until an exit syscall / ebreak / error, or the cycle budget
  // runs out; returns the exit code
  virtual int run() = 0;

  virtual void dumpRegs() const = 0;

  // The architectural value of register i (through the rename map, if
  // the core has one) — the differential checker compares final state
  virtual uint32_t reg(int i) const = 0;

  // Called once per retired instruction with its CommitRecord, in
  // retirement order. Null by default; the differential checker plugs
  // in here to compare the core against the functional reference model
  std::function<void(const CommitRecord &)> onCommit;
};
