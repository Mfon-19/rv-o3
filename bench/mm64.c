/*
 * 64x64 integer matrix multiply: three 16 KiB arrays, a 48 KiB
 * working set sized to straddle the L1 sweep points: it misses hard
 * at 16 KiB and fits at 64 KiB. The cache-capacity knee workload.
 */
#include "bench.h"

#define N 64u

static u32 A[N][N], B[N][N], C[N][N];

int main(void) {
    for (u32 i = 0; i < N; i++)
        for (u32 j = 0; j < N; j++) {
            A[i][j] = lcg() & 0xFFu;
            B[i][j] = lcg() & 0xFFu;
        }
    for (u32 i = 0; i < N; i++)
        for (u32 j = 0; j < N; j++) {
            u32 acc = 0;
            for (u32 k = 0; k < N; k++)
                acc += A[i][k] * B[k][j];
            C[i][j] = acc;
        }
    u32 sum = 0;
    for (u32 i = 0; i < N; i++)
        for (u32 j = 0; j < N; j++)
            sum ^= C[i][j] + i * 31u + j;
    print_int((int)sum);
    return 0;
}
