/*
 * Branch stress: 20000 iterations of LCG-driven branches with mixed
 * bias: one loop branch that is always taken, a 50/50 coin, a mildly
 * biased test, and a rare event. High MPKI and constant recovery
 * traffic.
 */
#include "bench.h"

#define ITERS 20000u

int main(void) {
    u32 a = 0, b = 0;
    for (u32 i = 0; i < ITERS; i++) {
        u32 x = lcg();
        /* The empty asm in each arm is an opaque side effect: the
         * compiler cannot convert these into branchless selects, so
         * the machine sees genuinely unpredictable branches */
        if (x & 1u) {
            a += x >> 3;
            __asm__ volatile("" : "+r"(a));
        } else {
            b ^= x;
            __asm__ volatile("" : "+r"(b));
        }
        if (x & 0x100u) {
            a ^= b >> 1;
            __asm__ volatile("" : "+r"(a));
        }
        if ((x & 0x7000u) == 0) {
            b += i;
            __asm__ volatile("" : "+r"(b));
        }
    }
    print_int((int)(a ^ b));
    return 0;
}
