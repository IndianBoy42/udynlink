/* MPS2-AN500 memory map (Cortex-M7)
 * ZBT SSRAM1 (Code):     4 MB @ 0x00000000
 * ZBT SSRAM 2&3 (Data):  4 MB @ 0x20000000
 * PSRAM:                16 MB @ 0x60000000
 *
 * For QEMU -kernel, the ELF is loaded at the addresses specified
 * in the program headers. We place everything in ZBT SSRAM1 for
 * simplicity; the LOT base at 0x20000000 is valid SSRAM 2&3.
 */

MEMORY
{
  RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 4M
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

  _end = .;
  _end_noinit = .;
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
