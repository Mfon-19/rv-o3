/* Guest ABI and host pipe protocol. All words are little-endian uint32_t.
 * ECALL 6: a0 -> {pixels, width, height, format[, palette]}.
 *   XRGB8888: pixels are 0x00RRGGBB words.
 *   INDEXED8: pixels are one byte each, indexing a 256-word 0x00RRGGBB
 *   palette named by the fifth descriptor word. The host expands them, as
 *   a display controller's palette DAC would; the frame pipe is XRGB.
 * ECALL 7: a0 <- key | (pressed << 8), or UINT32_MAX when no event exists.
 * Keys use ASCII plus the four arrow codes below. No terminal escape codes.
 * Frame pipe: {magic, width, height}, followed by width*height*4 pixel bytes.
 * Key pipe: one event word at a time. Neither pipe contains text/log output.
 */
#pragma once
#define RV_FRAME_MAGIC 0x31465652u /* RVF1 */
#define RV_PIXEL_XRGB8888 1u
#define RV_PIXEL_INDEXED8 2u
#define RV_MAX_WIDTH 640u
#define RV_MAX_HEIGHT 480u
#define RV_KEY_UP 0x80
#define RV_KEY_DOWN 0x81
#define RV_KEY_LEFT 0x82
#define RV_KEY_RIGHT 0x83
