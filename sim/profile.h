// Guest profiler (rvsim --profile): per guest pc, how many instructions
// retired there, how many cycles retirement waited for them, and how many
// were mispredicted branches. tools/guestprof.py sums these by function.
//
// Each retirement is charged the cycles since the previous retirement,
// i.e. the time the machine spent waiting to retire it. With a frame count
// given, recording starts once that many frames have been presented, so a
// game's startup can be left out.

#pragma once

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "core/commit.h"

class GuestProfile {
public:
  explicit GuestProfile(uint64_t afterFrames) : afterFrames(afterFrames) {}

  void record(const CommitRecord &rec, uint64_t cycle, uint64_t frames) {
    if (!started) {
      if (frames < afterFrames) {
        lastRetire = cycle;
        return;
      }
      started = true;
      start = lastRetire;
    }
    Counts &c = pcs[rec.pc];
    c.retired++;
    c.cycles += cycle - lastRetire;
    c.mispredicts += rec.mispredicted;
    lastRetire = cycle;
  }

  // Text: two # header lines, then "pc retired cycles mispredicts" rows
  bool write(const char *path, uint64_t frames) const {
    FILE *f = fopen(path, "w");
    if (!f)
      return false;
    std::vector<std::pair<uint32_t, Counts>> rows(pcs.begin(), pcs.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    uint64_t retired = 0;
    for (const auto &r : rows)
      retired += r.second.retired;
    fprintf(f,
            "# rvsim guest profile: frames %" PRIu64 "..%" PRIu64 ", %" PRIu64
            " retired, %" PRIu64 " cycles\n# pc retired cycles mispredicts\n",
            afterFrames, frames, retired, started ? lastRetire - start : 0);
    for (const auto &[pc, c] : rows)
      fprintf(f, "0x%08x %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", pc, c.retired,
              c.cycles, c.mispredicts);
    return fclose(f) == 0;
  }

private:
  struct Counts {
    uint64_t retired = 0, cycles = 0, mispredicts = 0;
  };
  std::unordered_map<uint32_t, Counts> pcs;
  uint64_t afterFrames, lastRetire = 0, start = 0;
  bool started = false;
};
