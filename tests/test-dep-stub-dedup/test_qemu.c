#include "udynlink.h"
#include "udynlink_deps.h"
#include "mod_math_module_data.h"
#include "mod_a_module_data.h"
#include "mod_b_module_data.h"
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

static int test_stub_dedup_single(udynlink_load_mode_t mode) {
    udynlink_module_t mod_math, mod_a, mod_b;
    int ok = 0;

    memset(&mod_math, 0, sizeof(mod_math));
    memset(&mod_a, 0, sizeof(mod_a));
    memset(&mod_b, 0, sizeof(mod_b));

    udynlink_dep_mgr_init(&g_dep_mgr, g_mod_entries, MAX_MODULES);
    udynlink_thunk_pool_init(&g_thunk_pool, g_thunk_buf, THUNK_POOL_SIZE);

    if (udynlink_dep_load(&g_dep_mgr, &mod_math, mod_math_module_data,
            NULL, 0, mode, &g_thunk_pool) != UDYNLINK_OK) {
        printf("mod_math load failed\n");
        return 0;
    }

    if (udynlink_dep_load(&g_dep_mgr, &mod_a, mod_a_module_data,
            NULL, 0, mode, &g_thunk_pool) != UDYNLINK_OK) {
        printf("mod_a load failed\n");
        goto cleanup_math;
    }

    if (udynlink_dep_load(&g_dep_mgr, &mod_b, mod_b_module_data,
            NULL, 0, mode, &g_thunk_pool) != UDYNLINK_OK) {
        printf("mod_b load failed\n");
        goto cleanup_a;
    }

    {
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod_a, "call_add_a", &sym) == NULL) {
            printf("call_add_a not found\n");
            ok = 0;
            goto cleanup_b;
        }
        int (*p_func)(int, int) = (int (*)(int, int))sym.val;
        UDYNLINK_PREPARE_CALL(&mod_a);
        int r = p_func(3, 4);
        if (r != 1007) {
            printf("call_add_a(3,4) = %d, expected 1007\n", r);
            ok = 0;
            goto cleanup_b;
        }
        printf("call_add_a(3,4) = 1007\n");
    }

    {
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod_b, "call_add_b", &sym) == NULL) {
            printf("call_add_b not found\n");
            ok = 0;
            goto cleanup_b;
        }
        int (*p_func)(int, int) = (int (*)(int, int))sym.val;
        UDYNLINK_PREPARE_CALL(&mod_b);
        int r = p_func(5, 6);
        if (r != 2011) {
            printf("call_add_b(5,6) = %d, expected 2011\n", r);
            ok = 0;
            goto cleanup_b;
        }
        printf("call_add_b(5,6) = 2011\n");
    }

    {
        size_t expected_used = UDYNLINK_STUB_SIZE;
        if (g_thunk_pool.used != expected_used) {
            printf("pool used = %zu, expected %zu (dedup failed)\n",
                   g_thunk_pool.used, expected_used);
            ok = 0;
            goto cleanup_b;
        }
        printf("pool used = %zu (1 stub shared by 2 modules)\n", g_thunk_pool.used);
    }

    printf("stub dedup: OK\n");
    ok = 1;

cleanup_b:
    udynlink_dep_unload(&g_dep_mgr, &mod_b);
cleanup_a:
    udynlink_dep_unload(&g_dep_mgr, &mod_a);
cleanup_math:
    udynlink_dep_unload(&g_dep_mgr, &mod_math);
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_stub_dedup_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
