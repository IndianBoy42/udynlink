#include "udynlink.h"
#include "mod_self_dep_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int test_circular_single(udynlink_load_mode_t mode) {
    const char *deps[4];
    size_t ndeps = udynlink_get_module_deps(mod_self_dep_module_data, deps, 4);
    if (ndeps != 1) {
        printf("expected 1 dep, got %zu\n", ndeps);
        return 0;
    }
    if (strcmp(deps[0], "mod_self_dep") != 0) {
        printf("expected dep 'mod_self_dep', got '%s'\n", deps[0]);
        return 0;
    }
    printf("deps read ok\n");

    udynlink_module_t mod;
    const udynlink_module_t *mod_deps[4];
    memset(&mod, 0, sizeof(mod));
    mod.deps = mod_deps;
    mod.max_deps = 4;
    udynlink_error_t err = udynlink_load_module(&mod, mod_self_dep_module_data, NULL, 0, mode);
    if (err == UDYNLINK_ERR_LOAD_CIRCULAR_DEP) {
        printf("circular dep detected ok\n");
        return 1;
    }
    printf("unexpected result: %s\n", udynlink_error_msg(&err));
    return 0;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_circular_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
