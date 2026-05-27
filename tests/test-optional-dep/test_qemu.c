#include "udynlink.h"
#include "mod_consumer_module_data.h"
#include "mod_logging_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int test_optional_dep_single(udynlink_load_mode_t mode) {
    udynlink_module_t consumer;
    udynlink_module_t logging;
    const udynlink_module_t *consumer_deps[4];
    int ok = 0;

    memset(&consumer, 0, sizeof(consumer));
    memset(&logging, 0, sizeof(logging));
    consumer.deps = consumer_deps;
    consumer.max_deps = 4;

    // Mark logging as "loading" so it returns DEFERRED
    udynlink_test_add_loading_name("mod_logging");

    // Defer the cross-module symbol so load doesn't fail
    udynlink_test_defer_symbol("log_get_value");

    // Load consumer (logging is deferred)
    if (test_load_module(&consumer, mod_consumer_module_data, NULL, 0, mode) != UDYNLINK_OK) {
        printf("consumer load failed\n");
        udynlink_test_clear_loading_names();
        return 0;
    }

    // Consumer should not be fully linked
    if (udynlink_is_module_fully_linked(&consumer)) {
        printf("consumer should not be fully linked\n");
        ok = 0;
        goto cleanup;
    }

    // Call consumer test in degraded mode
    {
        uintptr_t *mod_base = (uintptr_t *)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = consumer.ram_base;
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&consumer, "test", &sym) == NULL) {
            printf("test not found in consumer\n");
            ok = 0;
            goto cleanup;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        int result = p_func();
        if (result != 0) {
            printf("unexpected consumer value=%d\n", result);
            ok = 0;
            goto cleanup;
        }
        printf("consumer value=0\n");
    }
    printf("optional degraded ok\n");

    // Now load logging module for real
    if (test_load_module(&logging, mod_logging_module_data, NULL, 0, mode) != UDYNLINK_OK) {
        printf("logging load failed\n");
        ok = 0;
        goto cleanup;
    }

    // Clear deferred symbols so re-resolution can find them in deps
    udynlink_test_clear_deferred_symbols();

    // Manually link the optional dependency and incrementally resolve
    consumer.deps[consumer.num_deps++] = &logging;
    logging.dep_refcount++;
    if (udynlink_link_incremental(&consumer) != UDYNLINK_OK) {
        printf("link_incremental failed\n");
        ok = 0;
        goto cleanup;
    }

    // Consumer should now be fully linked
    if (!udynlink_is_module_fully_linked(&consumer)) {
        printf("consumer should now be fully linked\n");
        ok = 0;
        goto cleanup;
    }

    // Call consumer test again (should now use logging)
    {
        uintptr_t *mod_base = (uintptr_t *)UDYNLINK_LOT_BASE_ADDR;
        *mod_base = consumer.ram_base;
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&consumer, "test", &sym) == NULL) {
            printf("test not found in consumer\n");
            ok = 0;
            goto cleanup;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        int result = p_func();
        if (result != 123) {
            printf("unexpected consumer value=%d\n", result);
            ok = 0;
            goto cleanup;
        }
        printf("consumer value=123\n");
    }
    printf("optional linked ok\n");
    ok = 1;

cleanup:
    udynlink_test_clear_loading_names();
    if (logging.p_header) test_unload_module(&logging);
    if (consumer.p_header) test_unload_module(&consumer);
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_optional_dep_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
