/*
 * Memory map for Olimex STM32-H405 (STM32F405RG)
 * Flash: 1 MB @ 0x08000000
 * RAM:  192 KB @ 0x20000000
 */

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  /* QEMU models STM32F405 as 128K SRAM @ 0x20000000 plus 64K CCM @ 0x10000000.
   * RAM starts at 0x20000004 for 4-byte alignment padding. */
  RAM   (rwx) : ORIGIN = 0x20000004, LENGTH = 128K - 4
}

SECTIONS
{
  .isr_vector : {
    KEEP(*(.isr_vector))
  } > FLASH

  .text : {
    *(.text*)
  } > FLASH

  .rodata : {
    *(.rodata*)
  } > FLASH

  .ARM.exidx : {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } > FLASH

  _sidata = LOADADDR(.data);

  .data : {
    _sdata = .;
    *(.data*)
    _edata = .;
  } > RAM AT > FLASH

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
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
