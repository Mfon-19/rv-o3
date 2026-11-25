// Shared freestanding benchmark helpers: syscall print_int and deterministic LCG.

#pragma once

typedef unsigned int u32;

static __attribute__((noinline)) void print_int(int value) {
    register int a0 __asm__("a0") = value;
    register int a7 __asm__("a7") = 1;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

static u32 rng_state = 1u;

static u32 next_random(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}
