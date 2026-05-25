/* BBC micro:bit (nRF51822) memory map (Cortex-M0)
 * Flash: 256 KB @ 0x00000000
 * RAM:   16 KB @ 0x20000000
 *
 * For QEMU -kernel, the ELF is loaded at the addresses specified
 * in the program headers. Vector table must be at 0x00000000.
 */

MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 16K
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

  . = ALIGN(4);
  _sidata = .;

  .data : AT (_sidata) {
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
