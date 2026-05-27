/*
 * Memory map for Olimex STM32-H405 (STM32F405RG)
 * Flash: 1 MB @ 0x08000000
 * RAM:  192 KB @ 0x20000000
 */

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  /* QEMU models STM32F405 as 128K SRAM @ 0x20000000 plus 64K CCM @ 0x10000000.
   * We reserve the first 4 bytes of SRAM for the LOT base pointer so that
   * writing to UDYNLINK_LOT_BASE_ADDR (0x20000000) does not corrupt .data. */
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

  _end = .;
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
