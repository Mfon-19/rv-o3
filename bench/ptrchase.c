/*
 * Pointer chasing: the anti-MLP workload. A single cycle through 32768
 * nodes (512 KiB, twice the L2) visited in permuted order; every
 * load's address depends on the previous load's data, so misses
 * serialize to DRAM and no amount of out-of-order machinery can
 * overlap them. Contrast the average-outstanding-misses stat with
 * mlpbench's.
 */
#include "bench.h"

#define N 32768u
#define STEPS 60000u

static struct {
    u32 next;
    u32 pad[3]; /* 16-byte nodes: 4 per cache line */
} nodes[N];

int main(void) {
    for (u32 i = 0; i < N; i++)
        nodes[i].next = i;
    /* Sattolo's shuffle: links the nodes into a single ring that
     * visits every one of them before returning to the start */
    for (u32 i = N - 1; i > 0; i--) {
        u32 j = lcg() % i;
        u32 t = nodes[i].next;
        nodes[i].next = nodes[j].next;
        nodes[j].next = t;
    }
    u32 p = 0, sum = 0;
    for (u32 s = 0; s < STEPS; s++) {
        p = nodes[p].next;
        sum += p;
    }
    print_int((int)sum);
    return 0;
}
