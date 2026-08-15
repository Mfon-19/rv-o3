/*
 * Run-length encode/decode round trip over a 4 KiB buffer of random
 * runs: byte loads and stores, short data-dependent inner loops, and a
 * verified decompression.
 */
#include "bench.h"

#define N 4096u

static unsigned char src[N];
static unsigned char enc[2u * N + 8u];
static unsigned char dec[N];

int main(void) {
    u32 i = 0;
    while (i < N) {
        u32 len = 1u + (lcg() & 15u);
        unsigned char val = (unsigned char)(lcg() & 0xFFu);
        for (u32 k = 0; k < len && i < N; k++)
            src[i++] = val;
    }
    /* encode as (count, value) pairs, count 1..255 */
    u32 e = 0;
    i = 0;
    while (i < N) {
        u32 run = 1;
        while (i + run < N && src[i + run] == src[i] && run < 255u)
            run++;
        enc[e++] = (unsigned char)run;
        enc[e++] = src[i];
        i += run;
    }
    /* decode and verify */
    u32 d = 0;
    for (u32 k = 0; k < e; k += 2)
        for (u32 r = 0; r < enc[k]; r++)
            dec[d++] = enc[k + 1];
    u32 ok = (d == N);
    for (i = 0; i < N; i++)
        if (dec[i] != src[i])
            ok = 0;
    u32 sum = 0;
    for (i = 0; i < N; i++)
        sum = sum * 33u + dec[i];
    print_int((int)ok);
    print_int((int)e);
    print_int((int)sum);
    return 0;
}
