#include "udynlink.h"
#include "mod_globals1_module_data.h"
#include "test_utils.h"
#include <stdio.h>

#define EXPECTED_G_VAL                      60

int test_qemu(void) {
    const char *exported_syms[] = {"test", "g", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_globals1_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        CHECK_RAM_SIZE(&mod, 2 * sizeof(int));
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (!run_test_func(&mod))
            goto exit;
        // Check the expected value of the global variable
        uint32_t v = *(int*)udynlink_get_symbol_value(&mod, "g");
        if (v != EXPECTED_G_VAL) {
            printf("Unexpected value %d for variable 'g', expected %d\n", v, EXPECTED_G_VAL);
            goto exit;
        }
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
