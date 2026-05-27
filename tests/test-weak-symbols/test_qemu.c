#include "udynlink.h"
#include "mod_weak_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int g_override = 0;

static int host_weak_func(void) { return 99; }
static int host_weak_var = 88;

uint32_t test_resolve_symbol(const char *name) {
    if (g_override) {
        if (!strcmp(name, "weak_func"))
            return (uint32_t)(uintptr_t)&host_weak_func;
        if (!strcmp(name, "weak_var"))
            return (uint32_t)(uintptr_t)&host_weak_var;
    }
    return 0;
}

int test_qemu(void) {
    const char *exported_syms[] = {"test", NULL};
    const char *weak_syms[] = {"weak_func", "weak_var", NULL};
    const char *extern_syms[] = {NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int override = 0; override <= 1; override++) {
        g_override = override;
        for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
            if (udynlink_load_module(&mod, mod_weak_module_data, NULL, 0, (udynlink_load_mode_t)i))
                return 0;
            CHECK_RAM_SIZE(&mod, 0);
            if (!check_exported_symbols(&mod, exported_syms))
                goto exit;
            if (!check_weak_symbols(&mod, weak_syms))
                goto exit;
            if (!check_extern_symbols(&mod, extern_syms))
                goto exit;

            uint32_t* mod_base = (uint32_t*)UDYNLINK_LOT_BASE_ADDR;
            *mod_base = mod.ram_base;

            udynlink_sym_t sym;
            int expected_func = override ? 99 : 42;
            int expected_var = override ? 88 : 7;

            if (udynlink_lookup_symbol(&mod, "weak_func", &sym) == NULL) {
                printf("weak_func not found\n");
                goto exit;
            }
            int (*p_func)(void) = (int (*)(void))sym.val;
            if (p_func() != expected_func) {
                printf("weak_func returned %d, expected %d (override=%d)\n", p_func(), expected_func, override);
                goto exit;
            }

            if (udynlink_lookup_symbol(&mod, "weak_var", &sym) == NULL) {
                printf("weak_var not found\n");
                goto exit;
            }
            int *p_var = (int *)sym.val;
            if (*p_var != expected_var) {
                printf("weak_var is %d, expected %d (override=%d)\n", *p_var, expected_var, override);
                goto exit;
            }

            udynlink_unload_module(&mod);
        }
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
