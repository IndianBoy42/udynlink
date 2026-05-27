#include "udynlink.h"
#include "mod_provider_module_data.h"
#include "mod_consumer_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int test_dep_single(udynlink_load_mode_t mode) {
    const char *provider_exports[] = {"provider_add", "provider_mul", NULL};
    udynlink_module_t mod_provider;
    udynlink_module_t mod_consumer;
    int ok = 0;

    const udynlink_module_t *consumer_deps[4];

    memset(&mod_provider, 0, sizeof(mod_provider));
    memset(&mod_consumer, 0, sizeof(mod_consumer));
    mod_consumer.deps = consumer_deps;
    mod_consumer.max_deps = 4;

    if (test_load_module(&mod_provider, mod_provider_module_data, NULL, 0, mode))
        return 0;
    if (!check_exported_symbols(&mod_provider, provider_exports)) {
        test_unload_module(&mod_provider);
        return 0;
    }

    if (test_load_module(&mod_consumer, mod_consumer_module_data, NULL, 0, mode)) {
        test_unload_module(&mod_provider);
        return 0;
    }

    {
        uintptr_t *mod_base = (uintptr_t *)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = mod_consumer.ram_base;
        ok = run_test_func(&mod_consumer);
    }

    if (udynlink_unload_module(&mod_consumer) != UDYNLINK_OK)
        printf("consumer unload failed\n");
    if (udynlink_unload_module(&mod_provider) != UDYNLINK_OK)
        printf("provider unload failed\n");

    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_dep_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
