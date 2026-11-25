// Doomgeneric platform backend for rv-o3: deterministic virtual clock,
// framebuffer presentation via ECALL 6, and key polling via ECALL 7.

#include <stdint.h>
#include <stdlib.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "ecall.h"
#include "i_video.h"
#include "options.h"
#include "../sim/display_protocol.h"

static uint32_t ticks = 1;
static uint32_t frames = 0;

void DG_Init(void) {}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

// Deterministic virtual clock keeps gameplay independent of host simulation speed.
uint32_t DG_GetTicksMs(void) {
    return ticks++;
}

void DG_SleepMs(uint32_t ms) {
    ticks += ms;
}

void __real_I_Quit(void);

// Ensure clean exit status 0 when Doom quits from the menu or end of demo.
void __wrap_I_Quit(void) {
    __real_I_Quit();
    exit(0);
}

_Static_assert(sizeof(struct color) == 4, "palette entries must be XRGB words");

void DG_DrawFrame(void) {
    ++frames;

    // Pass framebuffer pointer, dimensions, format, and (in CMAP256 mode) palette
    // to rvsim so host-side palette expansion avoids 32-bit pixel conversion on guest.
    const uint32_t descriptor[] = {
        (uint32_t)(uintptr_t)DG_ScreenBuffer,
#ifdef CMAP256
        DOOMGENERIC_RESX, DOOMGENERIC_RESY, RV_PIXEL_INDEXED8,
        (uint32_t)(uintptr_t)colors
#else
        DOOMGENERIC_RESX, DOOMGENERIC_RESY, RV_PIXEL_XRGB8888
#endif
    };

    rv_call(6, (uint32_t)(uintptr_t)descriptor);

    if (MAX_FRAMES && frames >= MAX_FRAMES)
        exit(0);
}

static int translate_key(int key_char) {
    if (key_char >= 'A' && key_char <= 'Z')
        key_char += 'a' - 'A';

    switch (key_char) {
    case RV_KEY_UP:    return KEY_UPARROW;
    case RV_KEY_DOWN:  return KEY_DOWNARROW;
    case RV_KEY_LEFT:  return KEY_LEFTARROW;
    case RV_KEY_RIGHT: return KEY_RIGHTARROW;
    case 'w':          return KEY_UPARROW;
    case 's':          return KEY_DOWNARROW;
    case 'a':          return KEY_LEFTARROW;
    case 'd':          return KEY_RIGHTARROW;
    case 'j':          return KEY_STRAFE_L;
    case 'l':          return KEY_STRAFE_R;
    case 'f':          return KEY_FIRE;
    case 'e':
    case ' ':          return KEY_USE;
    case '\n':         return KEY_ENTER;
    case 'q':          exit(0);
    default:           return key_char;
    }
}

int DG_GetKey(int *pressed, unsigned char *key) {
    const uint32_t event = rv_call(7, 0);
    if (event == UINT32_MAX)
        return 0;

    *pressed = (event >> 8) & 1;
    *key = translate_key(event & 255);
    return 1;
}

int main(void) {
    char *args[] = {
        "doom", "-iwad", "game.wad", "-nosound", "-nogui",
#if PLAY_MODE
        "-warp", "1", "1",
#else
        "-playdemo", "demo1",
#endif
    };

    doomgeneric_Create(sizeof(args) / sizeof(args[0]), args);

    for (;;)
        doomgeneric_Tick();
}
