/* MPS2-AN385 memory map (Cortex-M3)
 * ZBT SSRAM1 (Code/Data): 8 MB @ 0x00000000
 * BRAM (Data):             4 MB @ 0x20000000
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
  /* Secondary placement pool for sectioned modules (see tests/qemu_host/src/main.c).
   * NOLOAD: it holds no initialized data, so it must not add a zero-filled image
   * to flash, and it must live in RAM. Declaring it here rather than letting the
   * linker treat it as an orphan section keeps it out of the load image and out
   * of whichever region orphan placement would have picked. */
  .altpool (NOLOAD) : ALIGN(32)
  {
    __altpool_start__ = .;
    *(.altpool .altpool.*)
    __altpool_end__ = .;
  } > RAM


  _end = .;
  _end_noinit = .;
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
