#include "udynlink.h"
#include "udynlink_externals.h"
#include "udynlink_host_utils.h"
#include "mod_a_module_data.h"
#include "mod_b_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int host_func_call_count = 0;

int host_func(int x) {
    return x * 10;
}

static uintptr_t real_resolver(const udynlink_module_t *p_mod, const char *name) {
    (void)p_mod;
    host_func_call_count++;
    if (!strcmp(name, "host_func"))
        return (uintptr_t)&host_func;
    return 0;
}

static udynlink_host_sym_cache_entry_t g_sym_cache[UDYNLINK_HOST_SYM_CACHE_SIZE];

// Override the weak test_resolve_symbol to plug in the cache
uintptr_t test_resolve_symbol(const char *name) {
    return udynlink_host_sym_cache_lookup(
        g_sym_cache, UDYNLINK_HOST_SYM_CACHE_SIZE, NULL, name, real_resolver);
}

int test_qemu(void) {
    udynlink_module_t mod_a, mod_b;
    int res = 0;

    memset(&mod_a, 0, sizeof(mod_a));
    memset(&mod_b, 0, sizeof(mod_b));

    if (test_load_module(&mod_a, mod_a_module_data, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL))
        return 0;

    // After loading mod_a, real_resolver should have been called once
    if (host_func_call_count != 1) {
        printf("Expected 1 resolver call after mod_a, got %d\n", host_func_call_count);
        goto exit;
    }

    if (test_load_module(&mod_b, mod_b_module_data, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL))
        goto exit;

    // After loading mod_b, real_resolver should STILL be at 1 (cache hit)
    if (host_func_call_count != 1) {
        printf("Expected 1 resolver call after mod_b (cache hit), got %d\n", host_func_call_count);
        goto exit;
    }

    if (!run_test_func(&mod_a))
        goto exit;
    if (!run_test_func(&mod_b))
        goto exit;

    printf("*** TEST OK *** cache_hits=%d\n", host_func_call_count);
    res = 1;

exit:
    test_unload_module(&mod_a);
    test_unload_module(&mod_b);
    return res;
}
