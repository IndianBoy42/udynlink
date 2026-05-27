#include "udynlink.h"
#include "mod_wasm2c_fac_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    const char *exported_syms[] = {"test", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_wasm2c_fac_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (!run_test_func(&mod))
            goto exit;
        udynlink_unload_module(&mod);
    }
    printf("*** TEST OK ***\n");
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
