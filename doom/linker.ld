/* 64 MiB flat RV32IM linker script for Doom: embedded WAD, Picolibc TLS, heap, and 1 MiB stack. */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
    RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 64M
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

    /* Advance .bss explicitly past .tbss because TLS NOBITS does not move `.`. */
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

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.riscv.attributes)
    }
}
