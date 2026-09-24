// Optional pipe bridge: the simulator has no dependency on a window library.
#pragma once
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include "sim/display_protocol.h"

namespace display {

inline int frameFd = -1, keyFd = -1;
inline uint8_t partialKey[4];
inline size_t keyBytes = 0;

[[noreturn]] inline void fail(const char *message) {
  fprintf(stderr, "display: %s\n", message);
  exit(1);
}

inline void configure(int frames, int keys) {
  if (frames < 0 && keys < 0) return;
  if (frames < 3 || keys < 3 || frames == keys)
    fail("--frame-fd and --key-fd must name distinct inherited descriptors >= 3");

  const int ff = fcntl(frames, F_GETFL), kf = fcntl(keys, F_GETFL);
  if (ff < 0 || kf < 0 || (ff & O_ACCMODE) == O_RDONLY ||
      (kf & O_ACCMODE) == O_WRONLY ||
      fcntl(keys, F_SETFL, kf | O_NONBLOCK) < 0)
    fail("invalid frame or key pipe");

  frameFd = frames;
  keyFd = keys;

  signal(SIGPIPE, SIG_IGN); // the launcher handles a viewer that closes
}
inline uint32_t readWord(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void appendWord(std::vector<uint8_t> &out, uint32_t v) {
  for (unsigned b = 0; b < 4; b++) out.push_back(uint8_t(v >> (8 * b)));
}

inline uint32_t pollKey() {
  if (keyFd < 0) return UINT32_MAX;

  while (keyBytes < 4) {
    const ssize_t n = read(keyFd, partialKey + keyBytes, 4 - keyBytes);

    if (n > 0) keyBytes += size_t(n);
    else if (n < 0 && errno == EINTR) continue;
    else if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
      return UINT32_MAX;
    else fail("key pipe read failed");
  }

  keyBytes = 0;
  const uint32_t event = readWord(partialKey);
  if (event > 0x1ff) fail("invalid key event");
  return event;
}
template <class ReadByte>
void present(uint32_t descriptor, size_t memoryBytes, ReadByte readByte, bool quiet) {
  auto check = [&](uint32_t addr, uint64_t size) {
    if (uint64_t(addr) + size > memoryBytes)
      fail("frame descriptor or pixels outside guest memory");
  };

  check(descriptor, 16);
  auto word = [&](uint32_t addr) {
    uint32_t v = 0;
    for (unsigned b = 0; b < 4; b++) v |= uint32_t(readByte(addr + b)) << (8 * b);
    return v;
  };

  const uint32_t pixels = word(descriptor), width = word(descriptor + 4),
                 height = word(descriptor + 8), format = word(descriptor + 12);
  if (!width || width > RV_MAX_WIDTH || !height || height > RV_MAX_HEIGHT ||
      (format != RV_PIXEL_XRGB8888 && format != RV_PIXEL_INDEXED8))
    fail("invalid dimensions or pixel format");

  const bool indexed = format == RV_PIXEL_INDEXED8;
  const size_t count = size_t(width) * height, bytes = count * 4;

  uint32_t palette = 0;
  if (indexed) {
    check(descriptor, 20);
    palette = word(descriptor + 16);
    check(palette, 256 * 4);
    check(pixels, count);
  } else
    check(pixels, bytes);

  if (quiet) return; // the reference validates the ABI but never presents twice
  if (frameFd < 0) fail("frame syscall requires --frame-fd and --key-fd");

  std::vector<uint8_t> packet;
  packet.reserve(12 + bytes);
  appendWord(packet, RV_FRAME_MAGIC);
  appendWord(packet, width);
  appendWord(packet, height);

  if (indexed) {
    uint8_t colors[256 * 4];

    for (uint32_t i = 0; i < sizeof colors; i++) colors[i] = readByte(palette + i);
    for (size_t i = 0; i < count; i++) {
      const uint8_t *c = colors + 4 * readByte(pixels + uint32_t(i));
      packet.insert(packet.end(), c, c + 4);
    }
  } else
    for (size_t i = 0; i < bytes; i++) packet.push_back(readByte(pixels + uint32_t(i)));

  size_t sent = 0;
  while (sent < packet.size()) {
    const ssize_t n = write(frameFd, packet.data() + sent, packet.size() - sent);
    
    if (n > 0) sent += size_t(n);
    else if (n < 0 && errno == EINTR) continue;
    else if (n < 0 && errno == EPIPE) return;
    else fail("frame pipe write failed");
  }
}
} // namespace display
