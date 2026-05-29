#include <stdint.h>

extern int math_add(int a, int b);

int call_add_a(int a, int b) {
    return math_add(a, b) + 1000;
}

int test(void) {
    return 0;
}
