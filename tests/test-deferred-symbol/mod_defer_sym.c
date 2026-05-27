#include <stddef.h>
#include <stdint.h>

extern void late_init(void);

int test(void) {
    void * volatile p = (void *)(uintptr_t)late_init;
    if (p != NULL) {
        ((void (*)(void))p)();
        return 2;
    }
    return 1;
}
