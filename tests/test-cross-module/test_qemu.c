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

static int test_cross_module_single(udynlink_load_mode_t mode) {
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
        if (udynlink_lookup_symbol(&mod_math, "math_add", &sym) == NULL) {
            printf("math_add not found in mod_math\n");
            ok = 0;
            goto cleanup_app;
        }
        int (*p_add)(int, int) = (int (*)(int, int))sym.val;
        UDYNLINK_PREPARE_CALL(&mod_math);
        int r = p_add(1, 2);
        if (r != 3) {
            printf("math_add(1,2) = %d, expected 3\n", r);
            ok = 0;
            goto cleanup_app;
        }
        printf("math_add(1, 2) = %d\n", r);
    }

    {
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod_math, "math_mul", &sym) == NULL) {
            printf("math_mul not found in mod_math\n");
            ok = 0;
            goto cleanup_app;
        }
        int (*p_mul)(int, int) = (int (*)(int, int))sym.val;
        UDYNLINK_PREPARE_CALL(&mod_math);
        int r = p_mul(3, 4);
        if (r != 12) {
            printf("math_mul(3,4) = %d, expected 12\n", r);
            ok = 0;
            goto cleanup_app;
        }
        printf("math_mul(3, 4) = %d\n", r);
    }

    {
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod_app, "call_math", &sym) == NULL) {
            printf("call_math not found in mod_app\n");
            ok = 0;
            goto cleanup_app;
        }
        int (*p_call)(int, int) = (int (*)(int, int))sym.val;
        UDYNLINK_PREPARE_CALL(&mod_app);
        int r = p_call(5, 3);
        if (r != 15) {
            printf("call_math(5,3) = %d, expected 15\n", r);
            ok = 0;
            goto cleanup_app;
        }
        printf("call_math(5, 3): sum=8, prod=15\n");
    }

    {
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod_app, "call_math", &sym) == NULL) {
            printf("call_math not found in mod_app (2nd call)\n");
            ok = 0;
            goto cleanup_app;
        }
        int (*p_call)(int, int) = (int (*)(int, int))sym.val;
        UDYNLINK_PREPARE_CALL(&mod_app);
        int r = p_call(10, 20);
        /* sum=30, prod=200, result = 30|200 = 222 */
        if (r != 222) {
            printf("call_math(10,20) = %d, expected 222\n", r);
            ok = 0;
            goto cleanup_app;
        }
        printf("call_math(10, 20): sum=30, prod=200\n");
    }

    printf("cross-module thunks: OK\n");
    ok = 1;

cleanup_app:
    udynlink_dep_unload(&g_dep_mgr, &mod_app);
cleanup_math:
    udynlink_dep_unload(&g_dep_mgr, &mod_math);
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_cross_module_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
