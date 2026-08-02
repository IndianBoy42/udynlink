#include <stdint.h>

/* The thunk macros live in udynlink_deps_api.h (module-facing half of
 * udynlink_deps.h); the test driver passes -I<repo>/udynlink so module
 * sources can include it. The slots survive --gc-sections via
 * KEEP(*(.bss.udynlink_thunk_pool)) in scripts/code_before_data.ld. */
#include "udynlink_deps_api.h"

/* math_add gets a preallocated in-module thunk (eager, no dynamic pool);
 * math_mul is intentionally undeclared so its import exercises the
 * dynamic-pool fallback. */
UDYNLINK_THUNK_GATEWAY();
UDYNLINK_THUNK_EXPORT(math_add);

int math_add(int a, int b) {
    return a + b;
}

int math_mul(int a, int b) {
    return a * b;
}

int test(void) {
    return 0;
}
