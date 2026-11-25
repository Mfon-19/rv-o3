// Dense 64x64 integer matrix multiply exercising multiplier throughput and
// L1D capacity (48 KiB working set across A, B, and C).

#include "bench.h"

#define DIM 64u

static u32 mat_a[DIM][DIM];
static u32 mat_b[DIM][DIM];
static u32 mat_c[DIM][DIM];

int main(void) {
    for (u32 i = 0; i < DIM; i++) {
        for (u32 j = 0; j < DIM; j++) {
            mat_a[i][j] = next_random() & 0xFFu;
            mat_b[i][j] = next_random() & 0xFFu;
        }
    }

    for (u32 i = 0; i < DIM; i++) {
        for (u32 j = 0; j < DIM; j++) {
            u32 acc = 0;
            for (u32 k = 0; k < DIM; k++)
                acc += mat_a[i][k] * mat_b[k][j];
            mat_c[i][j] = acc;
        }
    }

    u32 checksum = 0;
    for (u32 i = 0; i < DIM; i++) {
        for (u32 j = 0; j < DIM; j++)
            checksum ^= mat_c[i][j] + i * 31u + j;
    }

    print_int((int)checksum);
    return 0;
}
