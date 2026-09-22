/* Bulk reads from the embedded WAD avoid byte-at-a-time stdio overhead.
 * Keep the ordinary Doom lump cache so even unaligned WAD offsets are copied
 * into aligned guest allocations before the engine casts them to structs. */
#include <stdint.h>
#include <string.h>
#include "w_file.h"
#include "z_zone.h"

extern unsigned char _binary_game_wad_start[], _binary_game_wad_end[];
extern wad_file_class_t stdc_wad_file;

static wad_file_t *wad_open(char *path) {
    if (strcmp(path, "game.wad") && strcmp(path, "./game.wad")) return NULL;
    wad_file_t *wad = Z_Malloc(sizeof(*wad), PU_STATIC, NULL);
    memset(wad, 0, sizeof(*wad));
    wad->file_class = &stdc_wad_file;
    wad->length = (uintptr_t)_binary_game_wad_end - (uintptr_t)_binary_game_wad_start;
    return wad;
}
static void wad_close(wad_file_t *wad) { Z_Free(wad); }
static size_t wad_read(wad_file_t *wad, unsigned int offset, void *buffer, size_t length) {
    if (offset >= wad->length) return 0;
    if (length > wad->length - offset) length = wad->length - offset;
    memcpy(buffer, _binary_game_wad_start + offset, length);
    return length;
}
wad_file_class_t stdc_wad_file = {wad_open, wad_close, wad_read};
