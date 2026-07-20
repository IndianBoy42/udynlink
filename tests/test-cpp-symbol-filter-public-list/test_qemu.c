#include "udynlink.h"
#include "mod_public_list_module_data.h"
#include "test_utils.h"
#include <stdio.h>
int test_qemu(void) {
    udynlink_module_t mod;
    int res = 0;
    // The host allowlists only `a` and `c` via --public-symbols.
    // `b` and `d` are demoted to local and must NOT be findable.
    const char *listed[] = {"a", "c", "test", NULL};
    const char *not_listed[] = {"b", "d", NULL};
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (udynlink_load_module(&mod, mod_public_list_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        udynlink_cpp_init(&mod);
        CHECK_RAM_SIZE(&mod, 0);

        // Listed symbols must be present and EXPORTED.
        if (!check_exported_symbols(&mod, listed))
            goto exit;

        // Non-listed symbols must NOT be present at all (or, if they
        // appear, must not be EXPORTED).
        for (const char **p = not_listed; *p; p++) {
            if (is_exported_symbol(&mod, *p)) {
                printf("%s should NOT be exported but is\n", *p);
                goto exit;
            }
            printf("%s is NOT exported\n", *p);
        }
        printf("a is exported\n");
        printf("c is exported\n");

        if (!run_test_func(&mod))
            goto exit;
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
