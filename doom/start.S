/* RV32IM Doom entry point: sets sp and Picolibc TLS pointer tp, calls main, and exits. */
    .option norvc
    .option norelax
    .section .text.start, "ax", @progbits
    .globl _start
_start:
    la sp, __stack_top
    la tp, __tls_base
    call main
    li a7, 93
    ecall
1:  j 1b
