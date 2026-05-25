#include <stdint.h>
#include <string.h>

/* ARM Semihosting via BKPT 0xAB (Thumb-2) */
static void _sys_write0(const char *str) {
    __asm__ volatile(
        "movs r0, #4\n\t"      // SYS_WRITE0
        "movs r1, %0\n\t"
        "bkpt 0xAB\n\t"
        :
        : "r" (str)
        : "r0", "r1"
    );
}

static void _sys_exit(int code) {
    __asm__ volatile(
        "movs r0, #0x18\n\t"   // SYS_EXIT
        "movs r1, %0\n\t"
        "bkpt 0xAB\n\t"
        :
        : "r" (code)
        : "r0", "r1"
    );
}

#define OUTPUT_BUF_SIZE 512
static char output_buf[OUTPUT_BUF_SIZE];
static uint32_t output_pos = 0;

static void flush_output(void) {
    if (output_pos > 0) {
        output_buf[output_pos] = '\0';
        _sys_write0(output_buf);
        output_pos = 0;
    }
}

static void buf_putc(char c) {
    if (output_pos >= OUTPUT_BUF_SIZE - 1) {
        flush_output();
    }
    output_buf[output_pos++] = c;
}

int _write(int fd, const void *buf, int count) {
    (void)fd;
    const char *p = buf;
    for (int i = 0; i < count; i++) {
        buf_putc(p[i]);
        if (p[i] == '\n') {
            flush_output();
        }
    }
    return count;
}

int _close(int fd) {
    (void)fd;
    return -1;
}

int _lseek(int fd, int ptr, int dir) {
    (void)fd; (void)ptr; (void)dir;
    return 0;
}

int _read(int fd, void *buf, int count) {
    (void)fd; (void)buf; (void)count;
    return 0;
}

int _isatty(int fd) {
    (void)fd;
    return 1;
}

void *_sbrk(int incr) {
    extern char _end;
    static char *heap_end = 0;
    char *prev_heap_end;
    if (heap_end == 0) heap_end = &_end;
    prev_heap_end = heap_end;
    heap_end += incr;
    return (void *)prev_heap_end;
}

void _exit(int status) {
    flush_output();
    _sys_exit(status);
    while (1);
}

int _kill(int pid, int sig) {
    (void)pid; (void)sig;
    return -1;
}

int _getpid(void) {
    return 1;
}

int _fstat(int fd, void *st) {
    (void)fd; (void)st;
    return -1;
}
