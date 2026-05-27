#include "udynlink.h"
#include "udynlink_externals.h"
#include "mod_no_prologue_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

int test_qemu(void) {
    const char *exported_syms[] = {"test", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_no_prologue_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        // Verify the module header advertises no-prologue
        if (!udynlink_module_has_no_prologue(mod.p_header)) {
            printf("Module should have no-prologue flag set!\n");
            goto exit;
        }
        if (!run_test_func(&mod))
            goto exit;
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
