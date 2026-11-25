/* Flat 1 MiB RV32IM linker script placing _start at 0x0 and stack at top of RAM. */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 1M
}

__stack_top = ORIGIN(RAM) + LENGTH(RAM);

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

    ASSERT(__image_end <= (__stack_top - 0x1000),
           "demo image leaves too little stack space")

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.riscv.attributes)
    }
}
