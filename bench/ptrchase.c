// Latency-bound serial pointer-chasing benchmark over a shuffled 512 KiB cycle;
// each load depends on the previous one, so OoO execution cannot overlap misses.

#include "bench.h"

#define NODE_COUNT 32768u
#define STEPS 60000u

static struct {
    u32 next;
    u32 pad[3];
} nodes[NODE_COUNT];

int main(void) {
    for (u32 i = 0; i < NODE_COUNT; i++)
        nodes[i].next = i;

    // Fisher-Yates shuffle into a single random permutation.
    for (u32 i = NODE_COUNT - 1; i > 0; i--) {
        u32 swap_idx = next_random() % i;
        u32 temp = nodes[i].next;
        nodes[i].next = nodes[swap_idx].next;
        nodes[swap_idx].next = temp;
    }

    u32 current = 0;
    u32 sum = 0;

    for (u32 step = 0; step < STEPS; step++) {
        current = nodes[current].next;
        sum += current;
    }

    print_int((int)sum);
    return 0;
}
