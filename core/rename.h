// Register renaming: the rename map, the free list, and the physical
// register file.
//
// Every instruction that writes an architectural register is given a
// fresh physical register at rename; the map records which physical
// register currently speaks for each architectural one. Renaming is
// what deletes WAW and WAR hazards — two writers of the same
// architectural register write different physical registers, so only
// true RAW dependencies remain, tracked by the per-physical-register
// ready bits.
//
// Reclaim discipline (the part that grows bugs): allocating pdst for
// architectural rd displaces the previous mapping prevPhys. prevPhys
// is freed when the displacing instruction COMMITS (no older reader
// can still be in flight then, because commit is in order). On a
// squash, the walk restores map[rd] = prevPhys and returns pdst to
// the free list instead.
//
// x0 is permanently mapped to physical register 0, which holds zero
// and is never allocated.

#pragma once

#include <cstdint>
#include <vector>

struct PhysRegFile {
  std::vector<uint32_t> val;
  std::vector<uint8_t> ready; // value present (written back)?
  explicit PhysRegFile(uint32_t n) : val(n, 0), ready(n, 1) {}
};

struct FreeList {
  std::vector<uint8_t> q;
  uint32_t head = 0, n = 0;
  explicit FreeList(uint32_t cap) : q(cap) {}
  bool empty() const { return n == 0; }
  uint32_t count() const { return n; }
  uint8_t pop() {
    uint8_t r = q[head];
    head = (head + 1) % (uint32_t)q.size();
    n--;
    return r;
  }
  void push(uint8_t r) {
    q[(head + n) % (uint32_t)q.size()] = r;
    n++;
  }
};

struct RenameMap {
  uint8_t map[32];
  void reset() {
    for (int i = 0; i < 32; i++)
      map[i] = (uint8_t)i;
  }
};
