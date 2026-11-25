// Memory-level parallelism benchmark: independent random gathers over 512 KiB
// that overlap multiple cache misses via MSHRs and the out-of-order window.

#include "bench.h"

#define DATA_COUNT 131072u
#define INDEX_COUNT 32768u

static u32 data[DATA_COUNT];
static u32 indices[INDEX_COUNT];

int main(void) {
    for (u32 i = 0; i < DATA_COUNT; i++)
        data[i] = next_random();

    for (u32 i = 0; i < INDEX_COUNT; i++)
        indices[i] = next_random() & (DATA_COUNT - 1u);

    u32 sum = 0;
    for (u32 pass = 0; pass < 4; pass++) {
        for (u32 i = 0; i < INDEX_COUNT; i++)
            sum += data[indices[i]] + pass;
    }

    print_int((int)sum);
    return 0;
}
