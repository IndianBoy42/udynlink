#include "udynlink.h"
#include "mod_all_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    const char *exported_syms[] = {
        "test", "add", "sub", "mul",
        "call_mangled_through_got", "call_via_templated",
        "c_ctor_fn",
        NULL
    };
    // The module may resolve printf to puts depending on optimization;
    // either is acceptable.
    const char *extern_options[] = {"printf", "puts", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        udynlink_error_t err = udynlink_load_module(&mod, mod_all_module_data, NULL, 0, (udynlink_load_mode_t)i);
        if (err) {
            printf("load failed mode %d: %s\n", i, udynlink_error_msg(&err));
            return 0;
        }

        udynlink_cpp_init(&mod);
        printf("loaded mode %d\n", i);

        CHECK_RAM_SIZE(&mod, 0);
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        {
            int found = 0;
            for (const char **p = extern_options; *p; p++) {
                if (is_extern_symbol(&mod, *p)) { found = 1; break; }
            }
            if (!found) {
                printf("None of printf/puts found in externs\n");
                goto exit;
            }
        }
        if (!run_test_func(&mod))
            goto exit;
        printf("test ok mode %d\n", i);

        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
