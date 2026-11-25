// Guest ECALL syscall handler and host display/keyboard pipe bridge.
// Syscalls: 1=print int, 2=putchar, 3=print string, 4=print hex, 5=poll stdin,
// 6=present framebuffer, 7=poll key event, 10/93=exit.

#pragma once

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <vector>

#include "memory/memory.h"
#include "sim/display_protocol.h"

namespace display {

inline int frameFd = -1;
inline int keyFd = -1;
inline uint8_t partialKey[4];
inline size_t keyBytesRead = 0;

[[noreturn]] inline void fail(const char *message) {
  fprintf(stderr, "display: %s\n", message);
  exit(1);
}

// Validates inherited `--frame-fd` / `--key-fd` pipes and switches keyFd to nonblocking.
inline void configure(int frames, int keys) {
  if (frames < 0 && keys < 0)
    return;

  if (frames < 3 || keys < 3 || frames == keys)
    fail("--frame-fd and --key-fd must name distinct inherited descriptors >= 3");

  const int frameFlags = fcntl(frames, F_GETFL);
  const int keyFlags = fcntl(keys, F_GETFL);

  if (frameFlags < 0 || keyFlags < 0 ||
      (frameFlags & O_ACCMODE) == O_RDONLY ||
      (keyFlags & O_ACCMODE) == O_WRONLY ||
      fcntl(keys, F_SETFL, keyFlags | O_NONBLOCK) < 0) {
    fail("invalid frame or key pipe");
  }

  frameFd = frames;
  keyFd = keys;
  signal(SIGPIPE, SIG_IGN);
}

// Reads one 32-bit key event (`key | (pressed << 8)`) if available, else UINT32_MAX.
inline uint32_t pollKey() {
  if (keyFd < 0)
    return UINT32_MAX;

  while (keyBytesRead < 4) {
    const ssize_t bytesRead =
        read(keyFd, partialKey + keyBytesRead, 4 - keyBytesRead);

    if (bytesRead > 0) keyBytesRead += size_t(bytesRead);
    else if (bytesRead < 0 && errno == EINTR) continue;
    else if (bytesRead == 0 || errno == EAGAIN || errno == EWOULDBLOCK) return UINT32_MAX;
    else fail("key pipe read failed");
  }

  keyBytesRead = 0;
  const uint32_t event = readLittleEndian(partialKey, 4);
  if (event > 0x1ff)
    fail("invalid key event");

  return event;
}

// Reads a guest framebuffer descriptor via `readByte` (coherent through caches),
// expands 8-bit indexed pixels if requested, and writes an RVF1 packet to frameFd.
template <class ReadByte>
void present(uint32_t descriptorAddr, size_t memoryBytes, ReadByte readByte,
             bool quiet) {
  auto checkRange = [&](uint32_t addr, uint64_t size) {
    if (uint64_t(addr) + size > memoryBytes)
      fail("frame descriptor or pixels outside guest memory");
  };

  auto readGuestWord = [&](uint32_t addr) {
    uint8_t raw[4];
    for (unsigned i = 0; i < 4; i++)
      raw[i] = readByte(addr + i);
    return readLittleEndian(raw, 4);
  };

  checkRange(descriptorAddr, 16);

  const uint32_t pixelAddr = readGuestWord(descriptorAddr);
  const uint32_t width = readGuestWord(descriptorAddr + 4);
  const uint32_t height = readGuestWord(descriptorAddr + 8);
  const uint32_t format = readGuestWord(descriptorAddr + 12);

  if (!width || width > RV_MAX_WIDTH || !height || height > RV_MAX_HEIGHT ||
      (format != RV_PIXEL_XRGB8888 && format != RV_PIXEL_INDEXED8)) {
    fail("invalid dimensions or pixel format");
  }

  const bool isIndexed = format == RV_PIXEL_INDEXED8;
  const size_t pixelCount = size_t(width) * height;
  const size_t pixelBytes = pixelCount * 4;

  uint32_t paletteAddr = 0;
  if (isIndexed) {
    checkRange(descriptorAddr, 20);
    paletteAddr = readGuestWord(descriptorAddr + 16);
    checkRange(paletteAddr, 256 * 4);
    checkRange(pixelAddr, pixelCount);
  } else {
    checkRange(pixelAddr, pixelBytes);
  }

  if (quiet)
    return;

  if (frameFd < 0)
    fail("frame syscall requires --frame-fd and --key-fd");

  std::vector<uint8_t> packet(12);
  writeLittleEndian(packet.data(), 4, RV_FRAME_MAGIC);
  writeLittleEndian(packet.data() + 4, 4, width);
  writeLittleEndian(packet.data() + 8, 4, height);
  packet.reserve(12 + pixelBytes);

  if (isIndexed) {
    uint8_t palette[256 * 4];
    for (uint32_t i = 0; i < sizeof palette; i++)
      palette[i] = readByte(paletteAddr + i);

    for (size_t i = 0; i < pixelCount; i++) {
      const uint8_t *colorEntry = palette + 4 * readByte(pixelAddr + uint32_t(i));
      packet.insert(packet.end(), colorEntry, colorEntry + 4);
    }
  } else {
    for (size_t i = 0; i < pixelBytes; i++)
      packet.push_back(readByte(pixelAddr + uint32_t(i)));
  }

  size_t bytesSent = 0;
  while (bytesSent < packet.size()) {
    const ssize_t written =
        write(frameFd, packet.data() + bytesSent, packet.size() - bytesSent);

    if (written > 0) bytesSent += size_t(written);
    else if (written < 0 && errno == EINTR) continue;
    else if (written < 0 && errno == EPIPE) return;
    else fail("frame pipe write failed");
  }
}

}

inline bool syscallReturnsInput(uint32_t syscallNum) {
  return syscallNum == 5 || syscallNum == 7;
}

inline uint32_t pollInputByte() {
  pollfd input{STDIN_FILENO, POLLIN, 0};
  unsigned char byte = 0;

  if (poll(&input, 1, 0) > 0 && (input.revents & POLLIN) &&
      read(STDIN_FILENO, &byte, 1) == 1) {
    return byte;
  }

  return UINT32_MAX;
}

// Executes a guest ECALL. Returns true if the program requested termination.
template <class ReadByte>
bool runSyscall(uint32_t syscallNum, uint32_t &arg, uint32_t pc, bool quiet,
                ReadByte readByte, int &exitCode, size_t memoryBytes,
                uint32_t replayInput = UINT32_MAX) {
  switch (syscallNum) {
  case 1:
    if (!quiet) printf("%d\n", (int32_t)arg);
    return false;
  case 2:
    if (!quiet) putchar((int)(arg & 0xFF));
    return false;
  case 3:
    if (!quiet) {
      for (uint32_t addr = arg; readByte(addr) != 0; addr++)
        putchar(readByte(addr));
      fflush(stdout);
    }
    return false;
  case 4:
    if (!quiet) printf("%08x\n", arg);
    return false;
  case 5:
    arg = quiet ? replayInput : pollInputByte();
    return false;
  case 6:
    display::present(arg, memoryBytes, readByte, quiet);
    return false;
  case 7:
    arg = quiet ? replayInput : display::pollKey();
    return false;
  case 10:
  case 93:
    exitCode = (int)(arg & 0xFF);
    return true;
  default:
    if (!quiet)
      fprintf(stderr, "warning: unknown syscall a7=%u at pc=0x%08x\n", syscallNum, pc);
    return false;
  }
}
