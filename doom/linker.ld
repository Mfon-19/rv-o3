/*
 * RV32IM Doom: 64 MiB RAM, embedded read-only WAD, Picolibc TLS,
 * bounded heap, and a 1 MiB stack. rvsim zeroes RAM before loading.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

/* Keep this synchronized with run.py's -m argument. */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64M
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
        /* KEEP preserves the entry point if section garbage collection is enabled. */
        KEEP(*(.text.start))
        *(.text .text.*)
    } > RAM :text

    .rodata :
    {
        . = ALIGN(4);
        *(.rodata .rodata.* .srodata .srodata.*)
    } > RAM :text

    .data :
    {
        . = ALIGN(4);
        *(.data .data.*)
        *(.sdata .sdata.* .got .got.*)
    } > RAM :data

    .tdata ALIGN(16) :
    {
        __tls_base = .;
        *(.tdata .tdata.*)
    } > RAM :data

    .tbss ALIGN(16) (NOLOAD) :
    {
        *(.tbss .tbss.*)
    } > RAM :data

    /* TLS NOBITS does not advance the regular section cursor automatically. */
    .bss (ADDR(.tbss) + SIZEOF(.tbss)) (NOLOAD) :
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
    __heap_start = .;
    __heap_end = __stack_top - 0x100000;

    ASSERT(__heap_end - __heap_start >= 0x800000,
           "Doom image leaves less than 8 MiB for the heap")

    /* Metadata is useful in ELF files but has no meaning in the raw image. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.riscv.attributes)
    }
}
