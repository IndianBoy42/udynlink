#include <stdio.h>

__attribute__((weak)) int weak_func(void) {
    return 42;
}

__attribute__((weak)) int weak_var = 7;

int test(void) {
    /* Reference weak_var so --gc-sections keeps it */
    return weak_var >= 0;
}
