#include <stdint.h>

extern int nonexistent_func(void);

int trigger_missing(void) {
    return nonexistent_func();
}

int test(void) {
    return trigger_missing();
}
