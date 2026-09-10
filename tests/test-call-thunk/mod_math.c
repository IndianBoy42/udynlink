#include <stdint.h>

int math_add(int a, int b) {
    return a + b;
}

int math_mul(int a, int b) {
    return a * b;
}

/* Reads a module-global, which -fPIE -msingle-pic-base resolves through the
 * GOT (r9).  A gateway that hands the callee a garbage r9 makes this fault or
 * return garbage, so thunking it pins the gateway's ram_base load. */
int g_bias = 41;

int math_bias(void) {
    return g_bias;
}

int test(void) {
    return 0;
}
