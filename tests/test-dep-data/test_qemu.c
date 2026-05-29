#include "udynlink.h"
#include "udynlink_deps.h"
#include "mod_math_module_data.h"
#include "mod_app_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define THUNK_POOL_SIZE 512
#define MAX_MODULES     8

static uint8_t g_thunk_buf[THUNK_POOL_SIZE];
static udynlink_thunk_pool_t g_thunk_pool;

static udynlink_dep_entry_t g_mod_entries[MAX_MODULES];
static udynlink_dep_mgr_t g_dep_mgr;

udynlink_module_t *udynlink_external_dep_load(const char *name);
udynlink_module_t *udynlink_external_dep_load(const char *name) {
    static udynlink_module_t s_mod_math;
    if (strcmp(name, "mod_math") == 0) {
        if (udynlink_dep_load(&g_dep_mgr, &s_mod_math,
                mod_math_module_data, NULL, 0,
                UDYNLINK_LOAD_MODE_COPY_ALL, &g_thunk_pool) != UDYNLINK_OK) {
            return NULL;
        }
        return &s_mod_math;
    }
    return NULL;
}

uintptr_t test_resolve_symbol(const char *name);
uintptr_t test_resolve_symbol(const char *name) {
    if (udynlink_dep_is_dependency(name)) {
        return udynlink_dep_resolve_dependency(&g_dep_mgr, name);
    }
    uintptr_t thunk = udynlink_dep_resolve_func(&g_dep_mgr, &g_thunk_pool, name);
    if (thunk != 0) return thunk;
    uintptr_t data = udynlink_dep_resolve_data(&g_dep_mgr, name);
    if (data != 0) return data;
    return 0;
}

static int test_dep_data_single(udynlink_load_mode_t mode) {
    udynlink_module_t mod_math, mod_app;
    int ok = 0;

    memset(&mod_math, 0, sizeof(mod_math));
    memset(&mod_app, 0, sizeof(mod_app));

    udynlink_dep_mgr_init(&g_dep_mgr, g_mod_entries, MAX_MODULES);
    udynlink_thunk_pool_init(&g_thunk_pool, g_thunk_buf, THUNK_POOL_SIZE);

    if (udynlink_dep_load(&g_dep_mgr, &mod_math, mod_math_module_data,
            NULL, 0, mode, &g_thunk_pool) != UDYNLINK_OK) {
        printf("mod_math load failed\n");
        return 0;
    }

    if (udynlink_dep_load(&g_dep_mgr, &mod_app, mod_app_module_data,
            NULL, 0, mode, &g_thunk_pool) != UDYNLINK_OK) {
        printf("mod_app load failed\n");
        goto cleanup_math;
    }

    {
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod_app, "read_counter", &sym) == NULL) {
            printf("read_counter not found\n");
            ok = 0;
            goto cleanup_app;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        UDYNLINK_PREPARE_CALL(&mod_app);
        int r = p_func();
        if (r != 42) {
            printf("read_counter() = %d, expected 42\n", r);
            ok = 0;
            goto cleanup_app;
        }
        printf("read_counter() = 42\n");
    }

    printf("cross-module data: OK\n");
    ok = 1;

cleanup_app:
    udynlink_dep_unload(&g_dep_mgr, &mod_app);
cleanup_math:
    udynlink_dep_unload(&g_dep_mgr, &mod_math);
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_dep_data_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
