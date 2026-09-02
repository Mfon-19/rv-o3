// Register renaming: the rename map, the free list, and the physical
// register file.
//
// Every instruction that writes an architectural register (other than
// x0) is given a fresh physical register at rename; the map records
// which physical register currently speaks for each architectural one.
// No physical register is written twice while it is live, so the
// hazards caused by REUSING a register name (a write racing an older
// write, or a write racing an older read) simply cannot occur. The
// only dependence left is the real one: an instruction waiting for a
// value that has not been produced yet, tracked by the ready bit on
// each physical register.
//
// Reclaim discipline (the part that grows bugs): allocating pdst for
// architectural rd displaces the previous mapping prevPhys. prevPhys
// is freed when the displacing instruction COMMITS; commit is in
// order, so no older reader can still be in flight then. On a squash,
// the walk restores map[rd] = prevPhys and returns pdst to the free
// list instead.
//
// x0 is permanently mapped to physical register 0, which holds zero
// and is never allocated.

#pragma once

#include <cstdint>
#include <vector>

#include "core/ring.h"

struct PhysRegFile {
  std::vector<uint32_t> val;
  std::vector<uint8_t> ready; // value present (written back)?
  explicit PhysRegFile(uint32_t n) : val(n, 0), ready(n, 1) {}
};

// Physical registers not currently holding any live value, FIFO
using FreeList = Ring<uint8_t>;

struct RenameMap {
  uint8_t map[32];
  void reset() {
    for (int i = 0; i < 32; i++)
      map[i] = (uint8_t)i;
  }
};
