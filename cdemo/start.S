/* Freestanding RV32IM startup: initializes sp from linker.ld, calls main, and exits. */
    .option norvc
    .section .text.start, "ax", @progbits
    .globl _start
    .type _start, @function

_start:
    lui     sp, %hi(__stack_top)
    addi    sp, sp, %lo(__stack_top)
    call    main

    /* Pass main()'s return value in a0 to exit syscall 93. */
    addi    a7, zero, 93
    ecall

.Lhalt:
    jal     zero, .Lhalt

    .size _start, . - _start
