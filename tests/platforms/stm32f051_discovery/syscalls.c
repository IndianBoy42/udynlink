/*
 * Minimal newlib syscalls for STM32F103 QEMU test host.
 * Uses ARM semihosting via BKPT 0xAB.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

extern int errno;

/* -------------------------------------------------------------------------- */
/* Semihosting interface                                                      */
/* -------------------------------------------------------------------------- */

#define SEMIHOSTING_SYS_WRITE   0x05
#define SEMIHOSTING_SYS_READ    0x06
#define SEMIHOSTING_SYS_OPEN    0x01
#define SEMIHOSTING_SYS_CLOSE   0x02
#define SEMIHOSTING_SYS_FLEN    0x0C
#define SEMIHOSTING_SYS_SEEK    0x0A
#define SEMIHOSTING_SYS_EXIT    0x18

static inline int __attribute__((always_inline))
semihosting_call(int op, void *arg)
{
    int result;
    __asm volatile (
        "mov r0, %1\n"
        "mov r1, %2\n"
        "bkpt 0xAB\n"
        "mov %0, r0\n"
        : "=r" (result)
        : "r" (op), "r" (arg)
        : "r0", "r1", "r2", "r3", "memory"
    );
    return result;
}

/* -------------------------------------------------------------------------- */
/* _sbrk                                                                      */
/* -------------------------------------------------------------------------- */

extern char _end_noinit;
extern char __stack;

static char *heap_end = NULL;

caddr_t __attribute__((weak))
_sbrk(int incr)
{
    char *prev_heap_end;
    char *stack_ptr;

    if (heap_end == NULL) {
        heap_end = &_end_noinit;
    }

    prev_heap_end = heap_end;

    /* Rough stack pointer check: if heap would collide with stack, fail */
    __asm volatile ("mov %0, sp" : "=r" (stack_ptr));
    if (heap_end + incr > stack_ptr - 256) {
        errno = ENOMEM;
        return (caddr_t)-1;
    }

    heap_end += incr;
    return (caddr_t)prev_heap_end;
}

/* -------------------------------------------------------------------------- */
/* __initialize_args                                                          */
/* -------------------------------------------------------------------------- */

void __attribute__((weak))
__initialize_args(int *p_argc, char ***p_argv)
{
    static char name[] = "";
    static char *argv[2] = { name, NULL };
    *p_argc = 1;
    *p_argv = argv;
}

/* -------------------------------------------------------------------------- */
/* __initialize_hardware / __initialize_hardware_early                       */
/* -------------------------------------------------------------------------- */

void __attribute__((weak))
__initialize_hardware_early(void)
{
}

void __attribute__((weak))
__initialize_hardware(void)
{
}

/* -------------------------------------------------------------------------- */
/* _write / _read / _exit / _open / _close / _lseek / _fstat / _isatty       */
/* -------------------------------------------------------------------------- */

int __attribute__((weak))
_write(int file, char *ptr, int len)
{
    if (file != 1 && file != 2) {
        errno = EBADF;
        return -1;
    }
    volatile uint32_t block[3] = { (uint32_t)file, (uint32_t)ptr, (uint32_t)len };
    int not_written = semihosting_call(SEMIHOSTING_SYS_WRITE, (void *)block);
    return len - not_written;
}

int __attribute__((weak))
_read(int file, char *ptr, int len)
{
    if (file != 0) {
        errno = EBADF;
        return -1;
    }
    volatile uint32_t block[3] = { 0, (uint32_t)ptr, (uint32_t)len };
    return semihosting_call(SEMIHOSTING_SYS_READ, (void *)block);
}

void __attribute__((weak, noreturn))
_exit(int code)
{
    volatile uint32_t block[2] = { 0x20026, (uint32_t)code };
    semihosting_call(SEMIHOSTING_SYS_EXIT, (void *)block);
    while (1)
        ;
}

int __attribute__((weak))
_open(const char *name, int flags, int mode)
{
    (void)name; (void)flags; (void)mode;
    errno = ENOSYS;
    return -1;
}

int __attribute__((weak))
_close(int file)
{
    (void)file;
    errno = ENOSYS;
    return -1;
}

int __attribute__((weak))
_lseek(int file, int ptr, int dir)
{
    (void)file; (void)ptr; (void)dir;
    errno = ENOSYS;
    return -1;
}

int __attribute__((weak))
_fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int __attribute__((weak))
_isatty(int file)
{
    (void)file;
    return 1;
}
