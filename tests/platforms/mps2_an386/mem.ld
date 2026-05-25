/* MPS2-AN386 memory map (Cortex-M4)
 * ZBTSRAM (Code/Data): 8 MB @ 0x00000000
 * BRAM (Data):         4 MB @ 0x20000000
 *
 * For QEMU -kernel, the ELF is loaded at the addresses specified
 * in the program headers. We place everything in ZBTSRAM for
 * simplicity; the LOT base at 0x20000000 is valid BRAM.
 */

MEMORY
{
  RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 8M
}

SECTIONS
{
  .isr_vector : {
    KEEP(*(.isr_vector))
  } > RAM

  .text : {
    *(.text*)
  } > RAM

  .rodata : {
    *(.rodata*)
  } > RAM

  .data : {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > RAM

  .bss : {
    _sbss = .;
    *(.bss* COMMON)
    _ebss = .;
  } > RAM

  _end_noinit = .;
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
