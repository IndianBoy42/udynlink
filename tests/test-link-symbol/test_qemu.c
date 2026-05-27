#include "udynlink.h"
#include "mod_link_sym_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void real_service(void) {
    printf("real_service called\n");
}

static void mock_service(void) {
    printf("mock_service called\n");
}

// Override weak test_resolve_symbol to provide my_service
uint32_t test_resolve_symbol(const char *name);
uint32_t test_resolve_symbol(const char *name) {
    if (!strcmp(name, "my_service"))
        return (uint32_t)(uintptr_t)&real_service;
    return 0;
}

static int test_link_symbol_single(udynlink_load_mode_t mode) {
    udynlink_module_t mod;
    int ok = 0;

    memset(&mod, 0, sizeof(mod));

    // Load module (my_service resolves to real_service)
    if (test_load_module(&mod, mod_link_sym_module_data, NULL, 0, mode) != UDYNLINK_OK) {
        printf("mod load failed\n");
        return 0;
    }

    // Call test function (should call real_service)
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
            printf("unexpected result=%d\n", result);
            ok = 0;
            goto cleanup;
        }
    }

    // Patch my_service to mock_service
    if (udynlink_link_symbol(&mod, "my_service", (uint32_t)(uintptr_t)&mock_service) != UDYNLINK_OK) {
        printf("link_symbol failed\n");
        ok = 0;
        goto cleanup;
    }

    // Call test function again (should call mock_service)
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
            printf("unexpected result=%d\n", result);
            ok = 0;
            goto cleanup;
        }
    }

    printf("link symbol ok\n");
    ok = 1;

cleanup:
    if (mod.p_header) test_unload_module(&mod);
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_link_symbol_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
