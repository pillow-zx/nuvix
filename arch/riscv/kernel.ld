OUTPUT_ARCH(riscv)
ENTRY(_start)

BASE_ADDRESS = 0x80200000;
KERNEL_VBASE = 0xFFFFFFC000000000;

PHDRS
{
  text   PT_LOAD FLAGS(5);
  rodata PT_LOAD FLAGS(4);
  data   PT_LOAD FLAGS(6);
  bss    PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = KERNEL_VBASE + BASE_ADDRESS;

    .text : AT(ADDR(.text) - KERNEL_VBASE) {
        *(.text.entry)
        . = ALIGN(4096);
        KEEP(*(.text.secondary))
        . = ALIGN(4096);
        *(.text .text.*)
        . = ALIGN(4096);
    } :text

    .rodata : AT(ADDR(.rodata) - KERNEL_VBASE) {
        *(.rodata .rodata.*)
        . = ALIGN(4096);
    } :rodata

    .ex_table : AT(ADDR(.ex_table) - KERNEL_VBASE) {
        . = ALIGN(8);
        __start___ex_table = .;
        KEEP(*(__ex_table))
        __stop___ex_table = .;
        . = ALIGN(4096);
    } :rodata

    .data : AT(ADDR(.data) - KERNEL_VBASE) {
        *(.data .data.*)
        *(.sdata .sdata.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
    } :data

    .bss : AT(ADDR(.bss) - KERNEL_VBASE) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    } :bss

    . = ALIGN(4096);
    _end = .;
}
