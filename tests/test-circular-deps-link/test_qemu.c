#include "udynlink.h"
#include "mod_a_module_data.h"
#include "mod_b_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

static int test_circular_link_single(udynlink_load_mode_t mode) {
    udynlink_module_t mod_a;
    udynlink_module_t mod_b;
    const udynlink_module_t *a_deps[4];
    const udynlink_module_t *b_deps[4];
    int ok = 0;

    memset(&mod_a, 0, sizeof(mod_a));
    memset(&mod_b, 0, sizeof(mod_b));
    mod_a.deps = a_deps;
    mod_a.max_deps = 4;
    mod_b.deps = b_deps;
    mod_b.max_deps = 4;

    // Pre-register both as "loading" so the callback returns DEFERRED
    udynlink_test_add_loading_name("mod_a");
    udynlink_test_add_loading_name("mod_b");

    // Defer the cross-module symbols so load doesn't fail
    udynlink_test_defer_symbol("mod_b_get_value");

    // Load A (B is not loaded yet, so callback returns DEFERRED for B)
    if (test_load_module(&mod_a, mod_a_module_data, NULL, 0, mode) != UDYNLINK_OK) {
        printf("mod_a load failed\n");
        udynlink_test_clear_loading_names();
        return 0;
    }

    // Verify A is not fully linked (B deferred)
    if (udynlink_is_module_fully_linked(&mod_a)) {
        printf("mod_a should not be fully linked yet\n");
        ok = 0;
        goto cleanup;
    }

    // Load B (A is already loaded, so callback returns &mod_a)
    if (test_load_module(&mod_b, mod_b_module_data, NULL, 0, mode) != UDYNLINK_OK) {
        printf("mod_b load failed\n");
        ok = 0;
        goto cleanup;
    }

    // Verify B is not fully linked (A not in B's deps because A was already loaded... wait, no.
    // Actually B sees A as already loaded, so A IS linked into B immediately.
    // B should be fully linked.
    if (!udynlink_is_module_fully_linked(&mod_b)) {
        printf("mod_b should be fully linked\n");
        ok = 0;
        goto cleanup;
    }

    // Verify A does not have B in deps yet
    if (udynlink_get_linked_dependency(&mod_a, "mod_b") != NULL) {
        printf("mod_a should not have mod_b linked yet\n");
        ok = 0;
        goto cleanup;
    }

    // Verify B has A in deps
    if (udynlink_get_linked_dependency(&mod_b, "mod_a") == NULL) {
        printf("mod_b should have mod_a linked\n");
        ok = 0;
        goto cleanup;
    }

    // Clear deferred symbols so re-resolution can find them in deps
    udynlink_test_clear_deferred_symbols();

    // Link the deferred direction
    if (udynlink_link_dependency(&mod_a, &mod_b) != UDYNLINK_OK) {
        printf("link_dependency failed\n");
        ok = 0;
        goto cleanup;
    }

    // Verify A now has B in deps
    if (udynlink_get_linked_dependency(&mod_a, "mod_b") == NULL) {
        printf("mod_a should now have mod_b linked\n");
        ok = 0;
        goto cleanup;
    }

    // Verify both are fully linked
    if (!udynlink_is_module_fully_linked(&mod_a) || !udynlink_is_module_fully_linked(&mod_b)) {
        printf("both modules should be fully linked\n");
        ok = 0;
        goto cleanup;
    }

    // Verify refcounts
    if (mod_a.dep_refcount != 1 || mod_b.dep_refcount != 1) {
        printf("unexpected dep_refcount a=%u b=%u\n", mod_a.dep_refcount, mod_b.dep_refcount);
        ok = 0;
        goto cleanup;
    }

    // Call test functions
    {
        uintptr_t *mod_base = (uintptr_t *)UDYNLINK_LOT_BASE_ADDR;

        *mod_base = mod_a.ram_base;
        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod_a, "test", &sym) == NULL) {
            printf("test symbol not found in mod_a\n");
            ok = 0;
            goto cleanup;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        int result = p_func();
        if (result != 44) {
            printf("unexpected a_test=%d\n", result);
            ok = 0;
            goto cleanup;
        }
        printf("a_test=44\n");

        *mod_base = mod_b.ram_base;
        if (udynlink_lookup_symbol(&mod_b, "test", &sym) == NULL) {
            printf("test symbol not found in mod_b\n");
            ok = 0;
            goto cleanup;
        }
        p_func = (int (*)(void))sym.val;
        result = p_func();
        if (result != 11) {
            printf("unexpected b_test=%d\n", result);
            ok = 0;
            goto cleanup;
        }
        printf("b_test=11\n");
    }

    // Verify un-unloadable (circular refcount trap)
    if (udynlink_unload_module(&mod_a) != UDYNLINK_ERR_MODULE_HAS_DEPENDENTS) {
        printf("mod_a unload should fail (has dependents)\n");
        ok = 0;
        goto cleanup;
    }
    printf("unload blocked a\n");

    if (udynlink_unload_module(&mod_b) != UDYNLINK_ERR_MODULE_HAS_DEPENDENTS) {
        printf("mod_b unload should fail (has dependents)\n");
        ok = 0;
        goto cleanup;
    }
    printf("unload blocked b\n");

    ok = 1;
    printf("circular link ok\n");

cleanup:
    udynlink_test_clear_loading_names();
    return ok;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_circular_link_single((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
