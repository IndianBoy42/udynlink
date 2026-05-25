/*
 * Memory Spaces Definitions for STM32F051 (Cortex-M0).
 */

MEMORY
{
  RAM (xrw) : ORIGIN = 0x20000000, LENGTH = 8K
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K
}
