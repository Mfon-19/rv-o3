/*
 * A hosted executable normally receives startup code from libc (often called
 * crt0). rvsim has no OS or libc, so this file supplies the missing entry
 * point, initializes the stack, calls C main, and converts its return value
 * into the simulator's exit syscall.
 */

    /* rvsim implements RV32IM but not the compressed C extension. */
    .option norvc

    /* The linker script deliberately places this section before normal code. */
    .section .text.start, "ax", @progbits
    .globl _start
    .type _start, @function

_start:
    /*
     * __stack_top comes from linker.ld. The stack grows downward from the end
     * of rvsim's default 1 MiB memory and is 16-byte aligned as required by the
     * RISC-V ILP32 ABI.
     */
    lui     sp, %hi(__stack_top)
    addi    sp, sp, %lo(__stack_top)

    /* call establishes ra normally, so compiler-generated C can return here. */
    call    main

    /* main leaves its return value in a0; syscall 93 exits with that value. */
    addi    a7, zero, 93
    ecall

    /* Defensive fallback in case the simulator ever makes exit return. */
.Lhalt:
    jal     zero, .Lhalt

    .size _start, . - _start
