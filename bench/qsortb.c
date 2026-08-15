/*
 * Recursive quicksort of 512 random words: data-dependent branches,
 * recursion, and store/load traffic over a small working set.
 */
#include "bench.h"

#define N 512u

static u32 a[N];

static void sortrange(int lo, int hi) {
    if (lo >= hi)
        return;
    u32 pivot = a[(u32)(lo + (hi - lo) / 2)];
    int i = lo, j = hi;
    while (i <= j) {
        while (a[i] < pivot)
            i++;
        while (a[j] > pivot)
            j--;
        if (i <= j) {
            u32 t = a[i];
            a[i] = a[j];
            a[j] = t;
            i++;
            j--;
        }
    }
    sortrange(lo, j);
    sortrange(i, hi);
}

int main(void) {
    for (u32 i = 0; i < N; i++)
        a[i] = lcg();
    sortrange(0, (int)N - 1);
    u32 ok = 1, sum = 0;
    for (u32 i = 1; i < N; i++)
        if (a[i - 1] > a[i])
            ok = 0;
    for (u32 i = 0; i < N; i++)
        sum ^= a[i] + i;
    print_int((int)ok);
    print_int((int)sum);
    return 0;
}
