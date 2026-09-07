#include "udynlink.h"
#include "mod_wasm2c_calc_module_data.h"
#include "mod_wasm2c_calc_imports.h"
#include "test_utils.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Host context attached to the import env via calc_set_env_user(); the
 * module passes the env pointer back into every import call. */
static int g_env_ctx;
static int g_env_user_ok;

/* The import itself: wasm2c-named symbol (prototype comes from the
 * generated mod_wasm2c_calc_imports.h), resolved at module load time. */
u32 w2c_env_host_add(struct w2c_env* env, u32 a, u32 b) {
    g_env_user_ok = (env != NULL && env->user == (void*)&g_env_ctx);
    return a + b;
}

/* Phase 1 of each load mode flips this to 0 so the loader must reject the
 * module's import instead of binding a broken stub. */
static int g_allow_imports;

uintptr_t test_resolve_symbol(const char *name) {
    if (!g_allow_imports)
        return 0;
    if (!strcmp(name, "w2c_env_host_add"))
        return (uintptr_t)&w2c_env_host_add;
    return 0;
}

int test_qemu(void) {
    const char *extern_syms[] = {"w2c_env_host_add", NULL};
    const char *exported_syms[] = {"calc", "calc_set_env_user", NULL};
    udynlink_module_t mod;
    udynlink_sym_t sym;
    int res = 0;

    memset(&mod, 0, sizeof(mod));

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        /* 1. Unresolved import must fail the load cleanly.  The failure
         * path zeroes the module struct, so no unload is needed here. */
        g_allow_imports = 0;
        if (test_load_module(&mod, mod_wasm2c_calc_module_data, NULL, 0,
                             (udynlink_load_mode_t)i) == UDYNLINK_OK)
            goto exit;
        printf("unresolved import rejected\n");

        /* 2. With the import provided, the module loads and calls through. */
        g_allow_imports = 1;
        if (test_load_module(&mod, mod_wasm2c_calc_module_data, NULL, 0,
                             (udynlink_load_mode_t)i) != UDYNLINK_OK)
            goto exit;
        if (!check_extern_symbols(&mod, extern_syms) ||
            !check_exported_symbols(&mod, exported_syms))
            goto exit;

        /* Attach the env context, then run calc: 20*3 + host_add(3, 1). */
        if (udynlink_lookup_symbol(&mod, "calc_set_env_user", &sym) == NULL)
            goto exit;
        UDYNLINK_PREPARE_CALL(&mod);
        ((void (*)(void *))sym.val)(&g_env_ctx);

        if (udynlink_lookup_symbol(&mod, "calc", &sym) == NULL)
            goto exit;
        UDYNLINK_PREPARE_CALL(&mod);
        {
            u32 (*calc)(u32, u32) = (u32 (*)(u32, u32))sym.val;
            u32 r = calc(20, 3);
            printf("calc(20, 3) = %u\n", r);
            if (r != 64 || !g_env_user_ok)
                goto exit;
            printf("env user round-trip OK\n");
        }
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}