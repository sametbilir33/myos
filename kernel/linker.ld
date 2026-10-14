OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_main)

KERNEL_VMA = 0xffffffff80000000;
KERNEL_LMA = 0x100000;

PHDRS
{
    requests PT_LOAD;
    text PT_LOAD;
    rodata PT_LOAD;
    data PT_LOAD;
}

SECTIONS
{
    . = KERNEL_VMA;

    .limine_requests : AT(KERNEL_LMA)
    {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :requests

    . = ALIGN(4K);

    .text : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.text .text.*)
    } :text

    . = ALIGN(4K);

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(4K);

    .data : AT(ADDR(.data) - KERNEL_VMA + KERNEL_LMA)
    {
        *(.data .data.*)
    } :data

    .bss : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA)
    {
        *(COMMON)
        *(.bss .bss.*)
    } :data
}