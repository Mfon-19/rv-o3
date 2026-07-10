/*
 * Clang produces relocatable object files, but rvsim loads a flat image at
 * address zero. This script gives every section a final simulated address,
 * puts _start first, and defines the stack location used by start.S.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

/* Keep this synchronized with rvsim's default 1 MiB memory allocation. */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 1M
}

/* The stack begins one byte beyond RAM and grows downward into valid memory. */
__stack_top = ORIGIN(RAM) + LENGTH(RAM);

/*
 * ELF program headers keep code read/execute and data read/write. rvsim does
 * not enforce permissions, but expressing them correctly avoids an RWX ELF
 * segment and makes the intermediate file useful to normal inspection tools.
 */
PHDRS
{
    text PT_LOAD FLAGS(5); /* PF_R | PF_X */
    data PT_LOAD FLAGS(6); /* PF_R | PF_W */
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text :
    {
        /* KEEP ensures the entry point survives future section garbage collection. */
        KEEP(*(.text.start))
        *(.text .text.*)
    } > RAM :text

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata .rodata.*)
    } > RAM :text

    .data :
    {
        . = ALIGN(4);
        *(.data .data.*)
        *(.sdata .sdata.*)
    } > RAM :data

    .bss (NOLOAD) :
    {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        __bss_end = .;
    } > RAM :data

    . = ALIGN(16);
    __image_end = .;

    /* Reserve at least 4 KiB between the static image and the initial stack. */
    ASSERT(__image_end <= (__stack_top - 0x1000),
           "demo image leaves too little stack space")

    /* Metadata is useful in ELF files but has no meaning in the raw image. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.riscv.attributes)
    }
}
