#include <stddef.h>
#include <stdint.h>

extern int log_get_value(void);

int test(void) {
    void * volatile p = (void *)(uintptr_t)log_get_value;
    if (p != NULL) {
        return ((int (*)(void))p)();
    }
    return 0;
}
