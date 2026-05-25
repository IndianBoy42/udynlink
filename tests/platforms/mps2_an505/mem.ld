/* MPS2-AN505 memory map (Cortex-M33)
 * ZBT SSRAM1 (Code/Data): 4 MB @ 0x00000000
 * SRAM (Data):           128 KB @ 0x20000000
 *
 * For QEMU -kernel, the ELF is loaded at the addresses specified
 * in the program headers. We place everything in ZBT SSRAM1 for
 * simplicity; the LOT base at 0x20000000 is valid SRAM.
 */

MEMORY
{
  RAM (rwx) : ORIGIN = 0x10000000, LENGTH = 4M
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
