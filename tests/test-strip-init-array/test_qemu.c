#include "udynlink.h"
#include "mod_c_module_data.h"
#include "mod_cpp_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    udynlink_module_t mod_c, mod_cpp;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        // Load C module and verify it is smaller than the pre-filtering baseline
        if (udynlink_load_module(&mod_c, mod_c_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (sizeof(mod_c_module_data) > 220) {
            printf("C module image too large (%zu bytes), expected <= 220\n", sizeof(mod_c_module_data));
            goto exit;
        }
        // __init_array_start should NOT be present in a plain C module
        if (is_exported_symbol(&mod_c, "__init_array_start")) {
            printf("__init_array_start should not be in C module symbol table\n");
            goto exit;
        }
        if (!run_test_func(&mod_c))
            goto exit;
        udynlink_unload_module(&mod_c);

        // Load C++ module and verify __init_array_start is still present
        if (udynlink_load_module(&mod_cpp, mod_cpp_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!is_exported_symbol(&mod_cpp, "__init_array_start")) {
            printf("__init_array_start should be in C++ module symbol table\n");
            goto exit;
        }
        udynlink_cpp_init(&mod_cpp);
        if (!run_test_func(&mod_cpp))
            goto exit;
        udynlink_unload_module(&mod_cpp);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod_c);
    udynlink_unload_module(&mod_cpp);
    return res;
}
