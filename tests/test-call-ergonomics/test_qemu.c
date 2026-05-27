#include "udynlink.h"
#include "udynlink_call.h"
#include "mod_callergo_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    const char *exported_syms[] = {"add", "get_magic", "test", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (udynlink_load_module(&mod, mod_callergo_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        CHECK_RAM_SIZE(&mod, 0);
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;

        // Test 1: udynlink_resolve_func + UDYNLINK_CALL
        udynlink_func_t h_add, h_magic;
        if (udynlink_resolve_func(&mod, "add", &h_add) != UDYNLINK_OK) {
            printf("resolve_func add failed\n");
            goto exit;
        }
        if (udynlink_resolve_func(&mod, "get_magic", &h_magic) != UDYNLINK_OK) {
            printf("resolve_func get_magic failed\n");
            goto exit;
        }

        int r1 = UDYNLINK_CALL(&h_add, int, (2, 3));
        if (r1 != 5) {
            printf("UDYNLINK_CALL add(2,3) returned %d, expected 5\n", r1);
            goto exit;
        }

        int r2 = UDYNLINK_CALL(&h_magic, int, ());
        if (r2 != 0xCAFE) {
            printf("UDYNLINK_CALL get_magic() returned 0x%04X, expected 0xCAFE\n", r2);
            goto exit;
        }

        // Test 2: UDYNLINK_CALL_MODULE_FUNC one-shot
        int r3;
        udynlink_error_t err = UDYNLINK_CALL_MODULE_FUNC(&mod, "add", int, (10, 20), &r3);
        if (err != UDYNLINK_OK) {
            printf("CALL_MODULE_FUNC error %d\n", (int)err);
            goto exit;
        }
        if (r3 != 30) {
            printf("CALL_MODULE_FUNC add(10,20) returned %d, expected 30\n", r3);
            goto exit;
        }

        // Test 3: error path - unknown symbol
        int r4 = 99;
        err = UDYNLINK_CALL_MODULE_FUNC(&mod, "nonexistent", int, (), &r4);
        if (err != UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL) {
            printf("Expected UNKNOWN_SYMBOL, got %d\n", (int)err);
            goto exit;
        }
        if (r4 != 0) {
            printf("Expected r4=0 on failure, got %d\n", r4);
            goto exit;
        }

        // Also run the module's own test via the legacy helper
        if (!run_test_func(&mod))
            goto exit;

        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
