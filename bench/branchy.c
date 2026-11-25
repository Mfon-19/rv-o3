// Branch-prediction stress benchmark using unpredictable pseudo-random branches
// with inline asm barriers preventing the compiler from converting them to cmovs.

#include "bench.h"

#define ITERATIONS 20000u

int main(void) {
    u32 acc_a = 0;
    u32 acc_b = 0;

    for (u32 i = 0; i < ITERATIONS; i++) {
        u32 sample = next_random();

        if (sample & 1u) {
            acc_a += sample >> 3;
            __asm__ volatile("" : "+r"(acc_a));
        } else {
            acc_b ^= sample;
            __asm__ volatile("" : "+r"(acc_b));
        }

        if (sample & 0x100u) {
            acc_a ^= acc_b >> 1;
            __asm__ volatile("" : "+r"(acc_a));
        }

        if ((sample & 0x7000u) == 0) {
            acc_b += i;
            __asm__ volatile("" : "+r"(acc_b));
        }
    }

    print_int((int)(acc_a ^ acc_b));
    return 0;
}
