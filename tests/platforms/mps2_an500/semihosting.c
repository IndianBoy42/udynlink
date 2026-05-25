/* semihosting.c - ARM semihosting for MPS2-AN500 (mainline QEMU)
 *
 * Uses CMSDK UART at 0x4000_4000 (UART0 on MPS2 boards).
 * QEMU provides a working pl011-like UART at this address.
 */

#include <stdint.h>
#include <unistd.h>

/* CMSDK UART0 registers */
#define UART0_BASE   0x40004000
#define UART_DATA    (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define UART_STATE   (*(volatile uint32_t *)(UART0_BASE + 0x04))
#define UART_CTRL    (*(volatile uint32_t *)(UART0_BASE + 0x08))
#define UART_INT     (*(volatile uint32_t *)(UART0_BASE + 0x0C))
#define UART_BAUDDIV (*(volatile uint32_t *)(UART0_BASE + 0x10))

#define UART_STATE_TXFULL  (1 << 0)
#define UART_STATE_RXFULL  (1 << 1)
#define UART_CTRL_TX_EN    (1 << 0)
#define UART_CTRL_RX_EN    (1 << 1)
#define UART_CTRL_TX_INT   (1 << 2)
#define UART_CTRL_RX_INT   (1 << 3)
#define UART_CTRL_TXO_INT  (1 << 4)
#define UART_CTRL_RXO_INT  (1 << 5)
#define UART_CTRL_HST_INT  (1 << 6)

/* Initialize UART for TX */
static void uart_init(void) {
    UART_CTRL = UART_CTRL_TX_EN;
    /* QEMU doesn't need baud rate setup, but set a safe default */
    UART_BAUDDIV = 16;
}

/* Write a single character to UART */
static void uart_putc(char c) {
    while (UART_STATE & UART_STATE_TXFULL) {
        /* spin */
    }
    UART_DATA = c;
}

/* Minimal _write for newlib nano */
int _write(int fd, const char *buf, int len) {
    if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return -1;
    }
    for (int i = 0; i < len; i++) {
        uart_putc(buf[i]);
    }
    return len;
}

/* ARM semihosting - bkpt 0xAB interface */
static int semihosting_call(int op, void *arg) {
    int result;
    __asm__ volatile (
        "mov r0, %[op]\n"
        "mov r1, %[arg]\n"
        "bkpt #0xAB\n"
        "mov %[result], r0\n"
        : [result] "=r" (result)
        : [op] "r" (op), [arg] "r" (arg)
        : "r0", "r1", "memory"
    );
    return result;
}

#define SEMIHOSTING_SYS_WRITE0  0x04
#define SEMIHOSTING_SYS_EXIT    0x18

/* Write null-terminated string via semihosting */
void semihosting_write0(const char *str) {
    semihosting_call(SEMIHOSTING_SYS_WRITE0, (void *)str);
}

/* Exit via semihosting */
void semihosting_exit(int code) {
    uint32_t args[2] = {0x20026, (uint32_t)code};  /* ADP_Stopped_ApplicationExit */
    semihosting_call(SEMIHOSTING_SYS_EXIT, args);
}

/* Provide _sbrk for malloc */
extern char _end;
static char *heap_end = 0;

caddr_t _sbrk(int incr) {
    char *prev_heap_end;
    if (heap_end == 0) {
        heap_end = &_end;
    }
    prev_heap_end = heap_end;
    heap_end += incr;
    return (caddr_t)prev_heap_end;
}

/* Minimal _fstat to satisfy newlib */
int _fstat(int fd, void *st) {
    (void)fd;
    (void)st;
    return 0;
}

/* Minimal _isatty */
int _isatty(int fd) {
    (void)fd;
    return 1;
}

/* Minimal _lseek */
int _lseek(int fd, int ptr, int dir) {
    (void)fd;
    (void)ptr;
    (void)dir;
    return 0;
}

/* Minimal _close */
int _close(int fd) {
    (void)fd;
    return -1;
}

/* Minimal _read */
int _read(int fd, char *buf, int len) {
    (void)fd;
    (void)buf;
    (void)len;
    return 0;
}
