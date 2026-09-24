/* Doom rendering executes on RV32IM; the host presents finished frames. */
#include <stdint.h>
#include <stdlib.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "ecall.h"
#include "i_video.h"
#include "options.h"
#include "../sim/display_protocol.h"

static uint32_t ticks = 1, frames;

void DG_Init(void) {}
void DG_SetWindowTitle(const char *title) { (void)title; }
/* Virtual time keeps slow cycle simulation from accumulating real-time lag. */
uint32_t DG_GetTicksMs(void) { return ticks++; }
void DG_SleepMs(uint32_t ms) { ticks += ms; }

/* Doomgeneric's default I_Quit cleans up but returns when ORIGCODE is off.
 * End the guest too, both for menu quit and for the end of a single demo. */
void __real_I_Quit(void);
void __wrap_I_Quit(void) {
    __real_I_Quit();
    exit(0);
}

/* CMAP256 keeps Doom's 8-bit palette indices; the host expands them through
 * colors[], whose {b,g,r,a=0} entries are already 0x00RRGGBB words. */
_Static_assert(sizeof(struct color) == 4, "palette entries must be XRGB words");

void DG_DrawFrame(void) {
    ++frames;
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
    if (MAX_FRAMES && frames >= MAX_FRAMES) exit(0);
}

static int translate(int c) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    switch (c) {
    case RV_KEY_UP: return KEY_UPARROW;
    case RV_KEY_DOWN: return KEY_DOWNARROW;
    case RV_KEY_LEFT: return KEY_LEFTARROW;
    case RV_KEY_RIGHT: return KEY_RIGHTARROW;
    case 'w': return KEY_UPARROW;
    case 's': return KEY_DOWNARROW;
    case 'a': return KEY_LEFTARROW;
    case 'd': return KEY_RIGHTARROW;
    case 'j': return KEY_STRAFE_L;
    case 'l': return KEY_STRAFE_R;
    case 'f': return KEY_FIRE;
    case 'e': case ' ': return KEY_USE;
    case '\n': return KEY_ENTER;
    case 'q': exit(0);
    default: return c;
    }
}

int DG_GetKey(int *pressed, unsigned char *key) {
    const uint32_t event = rv_call(7, 0);
    if (event == UINT32_MAX) return 0;
    *pressed = (event >> 8) & 1;
    *key = translate(event & 255);
    return 1;
}

int main(void) {
    char *args[] = {"doom", "-iwad", "game.wad", "-nosound", "-nogui",
#if PLAY_MODE
                    "-warp", "1", "1",
#else
                    "-playdemo", "demo1",
#endif
    };
    doomgeneric_Create(sizeof(args) / sizeof(args[0]), args);
    for (;;) doomgeneric_Tick();
}
