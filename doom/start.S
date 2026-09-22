/* Flat image and RAM start zeroed by rvsim; initialized data is in the image. */
    .option norvc
    .option norelax
    .section .text.start, "ax", @progbits
    .globl _start
_start:
    la sp, __stack_top
    /* Picolibc keeps some C library state in TLS, even for this single thread. */
    la tp, __tls_base
    call main
    li a7, 93
    ecall
1:  j 1b
