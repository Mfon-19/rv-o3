#pragma once
#include <stdint.h>

static inline uint32_t rv_call(uint32_t number, uint32_t argument) {
    register uint32_t a0 __asm__("a0") = argument;
    register uint32_t a7 __asm__("a7") = number;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}
