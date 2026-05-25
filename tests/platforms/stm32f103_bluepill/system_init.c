/*
 * Minimal system init for STM32F103 in QEMU.
 * QEMU does not require clock configuration.
 */

void SystemInit(void) {
    /* No-op: QEMU is tolerant of default clock settings */
}
