/*
 * Dense integer matrix multiply, 24x24: the classic ILP and locality
 * kernel; the inner loops are full of multiplies, with reused rows
 * and strided column accesses.
 */
#include "bench.h"

#define N 24u

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
