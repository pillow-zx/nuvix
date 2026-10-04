OUTPUT_ARCH(riscv)
ENTRY(_start)

#include <nuvix/arch/riscv/pgtable.h>

PHDRS
{
	text   PT_LOAD FLAGS(5);
	rodata PT_LOAD FLAGS(4);
	data   PT_LOAD FLAGS(6);
	bss    PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = KERNEL_VBASE;

    __start = .;

	.text : 
    AT(KERNEL_PBASE + (ADDR(.text) - KERNEL_VBASE))
    {
        __text_start = .;
        KEEP(*(.text.boot))
        . = ALIGN(PAGE_SIZE);
        *(.text .text.*)
        . = ALIGN(PAGE_SIZE);
        __text_end = .;
	} : text

    .rodata ALIGN(PAGE_SIZE):
    AT(KERNEL_PBASE + (ADDR(.rodata) - KERNEL_VBASE))
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(PAGE_SIZE);
        __rodata_end = .;
    } : rodata

    .data ALIGN(PAGE_SIZE):
    AT(KERNEL_PBASE + (ADDR(.data) - KERNEL_VBASE))
    {
        __data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(PAGE_SIZE);
        __data_end = .;
	} : data

    .bss ALIGN(PAGE_SIZE) (NOLOAD): 
    AT(KERNEL_PBASE + (ADDR(.bss) - KERNEL_VBASE)) 
    {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(PAGE_SIZE);
        __bss_end = .;
	} : bss

    __end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }

    ASSERT(__start < __end, "empty kernel image")
    ASSERT(_start == __start, "boot entry must be first in the image")
}
