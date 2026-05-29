#include <stdint.h>
#include <stddef.h>

#define UDYNLINK_REQUIRES(mod_name) \
    const void *__udynlink_dep_##mod_name \
    __asm__(".udynlink.mod.requires." #mod_name) \
    __attribute__((used)) = 0

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
