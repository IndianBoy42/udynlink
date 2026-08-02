#include <stdint.h>
#include <stddef.h>

/* UDYNLINK_REQUIRES lives in udynlink_deps_api.h (module-facing half of
 * udynlink_deps.h); the test driver passes -I<repo>/udynlink. */
#include "udynlink_deps_api.h"

UDYNLINK_REQUIRES(mod_math);

extern int math_add(int a, int b);
extern int math_mul(int a, int b);

int call_math(int a, int b) {
    int sum = math_add(a, b);
    int prod = math_mul(a, b);
    return sum | prod;
}

int test(void) {
    return 0;
}
