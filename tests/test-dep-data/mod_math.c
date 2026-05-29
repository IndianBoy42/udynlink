#include <stdint.h>

int g_shared_counter = 42;
int math_add(int a, int b) {
    return a + b + g_shared_counter - 42;
}

int math_mul(int a, int b) {
    return a * b;
}

int test(void) {
    return 0;
}
