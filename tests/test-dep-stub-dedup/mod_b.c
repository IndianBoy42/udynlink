#include <stdint.h>

extern int math_add(int a, int b);

int call_add_b(int a, int b) {
    return math_add(a, b) + 2000;
}

int test(void) {
    return 0;
}
