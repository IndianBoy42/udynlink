/* MPS2-AN386 memory map (Cortex-M4)
 * ZBTSRAM (Code/Data): 8 MB @ 0x00000000
 * BRAM (Data):         4 MB @ 0x20000000
 *
 * For QEMU -kernel, the ELF is loaded at the addresses specified
 * in the program headers. We place everything in ZBTSRAM for
 * simplicity; the LOT base at 0x20000000 is valid BRAM.
 *
 * BRAM is linked in only when a test actually references it: the .altpool
 * output section (secondary placement pool for sectioned modules) is empty
 * for every other test, so the image layout stays unchanged.
 */

MEMORY
{
  RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 8M
  BRAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
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

  .ARM.exidx : {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
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

  /* Secondary placement pool: lives in the board's second RAM region. */
  .altpool (NOLOAD) : {
    __altpool_start__ = .;
    *(.altpool*)
    __altpool_end__ = .;
  } > BRAM

  _end = .;
  _end_noinit = .;
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}

