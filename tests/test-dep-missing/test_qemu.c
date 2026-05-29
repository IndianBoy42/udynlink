#include "udynlink.h"
#include "udynlink_deps.h"
#include "mod_missing_module_data.h"
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

int test_qemu(void) {
    udynlink_module_t mod;
    udynlink_error_t err;

    memset(&mod, 0, sizeof(mod));

    udynlink_dep_mgr_init(&g_dep_mgr, g_mod_entries, MAX_MODULES);
    udynlink_thunk_pool_init(&g_thunk_pool, g_thunk_buf, THUNK_POOL_SIZE);

    err = udynlink_dep_load(&g_dep_mgr, &mod, mod_missing_module_data,
            NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL, &g_thunk_pool);

    if (err != UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL) {
        printf("missing dep: expected LOAD_UNKNOWN_SYMBOL, got %d\n", err);
        udynlink_dep_unload(&g_dep_mgr, &mod);
        return 0;
    }

    printf("missing dep rejected: OK\n");
    return 1;
}
