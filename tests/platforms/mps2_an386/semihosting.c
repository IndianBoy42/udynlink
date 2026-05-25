/* semihosting.c - Custom semihosting layer for mainline QEMU
 *
 * Mainline QEMU's SYS_WRITE (buffered I/O) returns all bytes as
 * "unwritten", so stdout is silently swallowed.  We use SYS_WRITE0
 * (null-terminated string) and SYS_WRITEC (single char) instead.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

#define SEMIHOST_SYS_WRITEC     0x03
#define SEMIHOST_SYS_WRITE0     0x04
#define SEMIHOST_SYS_WRITE      0x05
#define SEMIHOST_SYS_EXIT       0x18

extern char _end_noinit[];
static char *heap_end = NULL;

static inline int __attribute__((always_inline))
semihost_call(int op, void *arg)
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

caddr_t _sbrk(int incr)
{
    char *prev_heap_end;
    char *stack_ptr;
    if (heap_end == NULL)
        heap_end = _end_noinit;
    prev_heap_end = heap_end;
    __asm volatile ("mov %0, sp" : "=r" (stack_ptr));
    if (heap_end + incr > stack_ptr - 256) {
        errno = ENOMEM;
        return (caddr_t)-1;
    }
    heap_end += incr;
    return (caddr_t)prev_heap_end;
}

void _exit(int code)
{
    /* SYS_EXIT_EXTENDED: { ADP_Stopped_ApplicationExit, exit_code } */
    volatile unsigned block[2] = { 0x20026, (unsigned)code };
    semihost_call(SEMIHOST_SYS_EXIT, (void *)block);
    while (1);
}

int _write(int file, char *ptr, int len)
{
    int i;
    (void)file;
    for (i = 0; i < len; i++) {
        semihost_call(SEMIHOST_SYS_WRITEC, &ptr[i]);
    }
    return len;
}

int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

int _close(int file)
{
    (void)file;
    return -1;
}

int _lseek(int file, int ptr, int dir)
{
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

int _open(const char *name, int flags, int mode)
{
    (void)name;
    (void)flags;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    errno = ENOSYS;
    return -1;
}

void __initialize_hardware_early(void) {}
void __initialize_hardware(void) {}
