#include <stdint.h>

/* UDYNLINK_REQUIRES lives in udynlink_deps_api.h (module-facing half of
 * udynlink_deps.h); the test driver passes -I<repo>/udynlink. */
#include "udynlink_deps_api.h"

UDYNLINK_REQUIRES(mod_b);

extern int mod_b_func(void);

int mod_a_func(void) {
    return 10;
}

int test(void) {
    return 0;
}
