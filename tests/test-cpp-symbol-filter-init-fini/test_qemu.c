#include "udynlink.h"
#include "mod_init_fini_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    const char *exported_syms[] = {"test", "c_ctor_first", "c_ctor_last", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        udynlink_error_t err = udynlink_load_module(&mod, mod_init_fini_module_data, NULL, 0, (udynlink_load_mode_t)i);
        if (err) {
            printf("load failed: %s\n", udynlink_error_msg(&err));
            return 0;
        }
        udynlink_cpp_init(&mod);
        CHECK_RAM_SIZE(&mod, 0);
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (!run_test_func(&mod))
            goto exit;
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
