#include <stdint.h>

/* UDYNLINK_REQUIRES lives in udynlink_deps_api.h (module-facing half of
 * udynlink_deps.h); the test driver passes -I<repo>/udynlink. */
#include "udynlink_deps_api.h"

UDYNLINK_REQUIRES(mod_a);

extern int mod_a_func(void);

int mod_b_func(void) {
    return 20;
}

int test(void) {
    return 0;
}
