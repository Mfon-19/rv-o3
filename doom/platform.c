/* All rendering, including the conversion to ASCII, executes on RV32IM. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "ecall.h"
#include "options.h"

static uint32_t ticks = 1, frames;
static unsigned char held[256];
static unsigned int expires[256];
static int escape_state;

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

void DG_DrawFrame(void) {
    static const char ramp[] = " .:-=+*#%@";
    static char output[(ASCII_COLS + 1) * ASCII_ROWS + 128];
    char *p = output;
    ++frames;
    if (ANSI_OUTPUT) {
        p += sprintf(p, "%s\033[H", frames == 1 ? "\033[2J" : "");
    } else {
        p += sprintf(p, "\n--- frame %u ---\n", (unsigned)frames);
    }
    for (unsigned y = 0; y < ASCII_ROWS; ++y) {
        unsigned y0 = y * DOOMGENERIC_RESY / ASCII_ROWS;
        unsigned y1 = (y + 1) * DOOMGENERIC_RESY / ASCII_ROWS;
        for (unsigned x = 0; x < ASCII_COLS; ++x) {
            unsigned x0 = x * DOOMGENERIC_RESX / ASCII_COLS;
            unsigned x1 = (x + 1) * DOOMGENERIC_RESX / ASCII_COLS;
            unsigned light = 0;
            for (unsigned sy = y0; sy < y1; ++sy) {
                for (unsigned sx = x0; sx < x1; ++sx) {
                    uint32_t rgb = DG_ScreenBuffer[sy * DOOMGENERIC_RESX + sx];
                    light += (77 * ((rgb >> 16) & 255) +
                              150 * ((rgb >> 8) & 255) + 29 * (rgb & 255)) >> 8;
                }
            }
            light /= (x1 - x0) * (y1 - y0);
            /* A modest brightness lift makes dark corridors legible in text. */
            light = light * 3 / 2;
            if (light > 255) light = 255;
            *p++ = ramp[light * (sizeof(ramp) - 2) / 255];
        }
        *p++ = '\n';
    }
    int status = snprintf(p, ASCII_COLS + 1,
                          "frame %u | WASD move/turn  J/L strafe  F fire  E use  Q quit",
                          (unsigned)frames);
    p += status < ASCII_COLS ? status : ASCII_COLS;
    *p++ = '\n';
    *p = 0;
    rv_print(output);
    if (MAX_FRAMES && frames >= MAX_FRAMES) exit(0);
}

static int translate(int c) {
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    switch (c) {
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
    /* Terminals send presses/repeats, not releases. Release after two frames. */
    for (unsigned i = 0; i < 256; ++i) {
        if (held[i] && frames >= expires[i]) {
            held[i] = 0;
            *pressed = 0; *key = i;
            return 1;
        }
    }
    for (;;) {
        int c = (int)rv_call(5, 0);
        if (c < 0) {
            if (escape_state == 1) {
                escape_state = 0;
                c = KEY_ESCAPE;
            } else return 0;
        } else if (escape_state == 1 && c == '[') {
            escape_state = 2;
            continue;
        } else if (escape_state == 2) {
            escape_state = 0;
            switch (c) {
            case 'A': c = KEY_UPARROW; break;
            case 'B': c = KEY_DOWNARROW; break;
            case 'C': c = KEY_RIGHTARROW; break;
            case 'D': c = KEY_LEFTARROW; break;
            default: continue;
            }
        } else if (c == 27) {
            escape_state = 1;
            continue;
        } else {
            escape_state = 0;
            c = translate(c);
        }
        expires[c] = frames + 2;
        if (held[c]) continue;
        held[c] = 1;
        *pressed = 1; *key = c;
        return 1;
    }
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
