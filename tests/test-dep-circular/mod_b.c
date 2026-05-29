#include <stdint.h>

#define UDYNLINK_REQUIRES(mod_name) \
    typedef void (*_udynlink_dep_fn_##mod_name)(void); \
    _udynlink_dep_fn_##mod_name _udynlink_dep_##mod_name \
        __asm__(".udynlink.mod.requires." #mod_name); \
    __attribute__((used)) void _udynlink_dep_ref_##mod_name(void) { \
        volatile _udynlink_dep_fn_##mod_name f = _udynlink_dep_##mod_name; \
        (void)f; \
    }

UDYNLINK_REQUIRES(mod_a);

extern int mod_a_func(void);

int mod_b_func(void) {
    return 20;
}

int test(void) {
    return 0;
}
