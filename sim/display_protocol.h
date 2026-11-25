// Shared framebuffer and key-event ABI between rvsim, guest code, and doom-viewer.

#pragma once

#define RV_FRAME_MAGIC 0x31465652u // "RVF1" little-endian packet header.

#define RV_PIXEL_XRGB8888 1u
#define RV_PIXEL_INDEXED8 2u

#define RV_MAX_WIDTH 640u
#define RV_MAX_HEIGHT 480u

// Non-ASCII arrow key codes encoded into the low byte of syscall 7 events.
#define RV_KEY_UP 0x80
#define RV_KEY_DOWN 0x81
#define RV_KEY_LEFT 0x82
#define RV_KEY_RIGHT 0x83
