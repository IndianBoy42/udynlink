#include "udynlink.h"
#include "mod_defer_sym_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static void real_late_init(void) {
    printf("late_init called\n");
}

static int test_deferred_single(udynlink_load_mode_t mode) {
    udynlink_module_t mod;
    int ok = 0;

    memset(&mod, 0, sizeof(mod));

    // Defer late_init symbol
    udynlink_test_defer_symbol("late_init");

    // Load module (should succeed despite deferred symbol)
    if (test_load_module(&mod, mod_defer_sym_module_data, NULL, 0, mode) != UDYNLINK_OK) {
        printf("mod load failed\n");
        udynlink_test_clear_deferred_symbols();
        return 0;
    }

    printf("deferred load ok\n");

    // Call test function (should return 1 because late_init is NULL)
    {
        uint32_t *mod_base = (uint32_t *)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = mod.ram_base;
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod, "test", &sym) == NULL) {
            printf("test not found\n");
            ok = 0;
            goto cleanup;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        int result = p_func();
        if (result != 1) {
            printf("unexpected before=%d\n", result);
            ok = 0;
            goto cleanup;
        }
        printf("deferred before=1\n");
    }

    // Patch the symbol directly
    if (udynlink_link_symbol(&mod, "late_init", (uint32_t)(uintptr_t)&real_late_init) != UDYNLINK_OK) {
        printf("link_symbol failed\n");
        ok = 0;
        goto cleanup;
    }

    // Call test function again (should return 2 and print "late_init called")
    {
        uint32_t *mod_base = (uint32_t *)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = mod.ram_base;
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod, "test", &sym) == NULL) {
            printf("test not found\n");
            ok = 0;
            goto cleanup;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        int result = p_func();
        if (result != 2) {
            printf("unexpected after=%d\n", result);
            ok = 0;
            goto cleanup;
        }
        printf("deferred after=2\n");
    }

    ok = 1;

cleanup:
    udynlink_test_clear_deferred_symbols();
    if (mod.p_header) test_unload_module(&mod);
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_deferred_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
