// The simulator's syscall interface: a7 = number, a0 = argument.
//
//   1   print a0 as a signed decimal integer, followed by a newline
//   2   print a0 as a single ASCII character
//   3   print the NUL-terminated string at address a0
//   4   print a0 as 8 hex digits (architectural-test signatures)
//   93  exit with code a0 (Linux-flavoured number; 10 also accepted)
//
// The core and the reference model run this same function, so the two
// cannot disagree about it. readByte fetches program memory: the core
// reads through its cache hierarchy, the reference model straight from
// its own memory. quiet drops the output (the silent shadow in a
// differential check) while still honoring exit. Returns true if the
// program asked to exit, with the status left in exitCode.

#pragma once

#include <cstdint>
#include <cstdio>

template <class ReadByte>
bool runSyscall(uint32_t num, uint32_t arg, uint32_t pc, bool quiet,
                ReadByte readByte, int &exitCode) {
  switch (num) {
  case 1:
    if (!quiet)
      printf("%d\n", (int32_t)arg);
    return false;
  case 2:
    if (!quiet)
      putchar((int)(arg & 0xFF));
    return false;
  case 3:
    if (!quiet)
      for (uint32_t a = arg; readByte(a) != 0; a++)
        putchar(readByte(a));
    return false;
  case 4:
    if (!quiet)
      printf("%08x\n", arg);
    return false;
  case 10:
  case 93:
    exitCode = (int)(arg & 0xFF);
    return true;
  default:
    if (!quiet)
      fprintf(stderr, "warning: unknown syscall a7=%u at pc=0x%08x\n", num,
              pc);
    return false;
  }
}
