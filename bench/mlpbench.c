/*
 * Memory-level parallelism: independent random gathers over a 512 KiB
 * array (twice the L2). Each load's address comes from a small index
 * table, not from the previous load, so the out-of-order window can
 * keep several DRAM misses in flight. This is ptrchase's control
 * experiment: average outstanding misses should clearly exceed its.
 */
#include "bench.h"

#define N 131072u /* data words: 512 KiB */
#define M 32768u  /* gathers */

static u32 data[N];
static u32 idx[M];

int main(void) {
    for (u32 i = 0; i < N; i++)
        data[i] = lcg();
    for (u32 i = 0; i < M; i++)
        idx[i] = lcg() & (N - 1u);
    /* four passes so the gather phase, not the LCG init, dominates
     * the run; the 512 KiB working set misses on every pass */
    u32 sum = 0;
    for (u32 pass = 0; pass < 4; pass++)
        for (u32 i = 0; i < M; i++)
            sum += data[idx[i]] + pass;
    print_int((int)sum);
    return 0;
}
