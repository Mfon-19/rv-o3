/*
 * Shared scaffolding for the benchmark programs. Each benchmark builds
 * two ways from the same source:
 *   - cross-compiled for rvsim (bare metal, cdemo runtime), printing
 *     through the simulator's ecall interface;
 *   - natively with -DHOST, printing through stdio.
 * The host binary is an independent oracle: it shares no code with the
 * simulator or its reference model, so comparing their outputs checks
 * the ISA semantics themselves. Everything is uint32 arithmetic so C
 * gives both targets identical answers.
 */
#pragma once

typedef unsigned int u32;

#ifdef HOST
#include <stdio.h>
static void print_int(int value) { printf("%d\n", value); }
#else
static __attribute__((noinline)) void print_int(int value) {
    register int a0 __asm__("a0") = value;
    register int a7 __asm__("a7") = 1;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}
#endif

static u32 lcg_state = 1u;
static u32 lcg(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}
