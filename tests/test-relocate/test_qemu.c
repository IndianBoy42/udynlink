#include "udynlink.h"
#include "udynlink_externals.h"
#include "mod_relocate_module_data.h"
#include "test_utils.h"
#include <stdio.h>

/* Helpers to call a module function by symbol name. */
static int call_void_func(const udynlink_module_t *p_mod, const char *name) {
    udynlink_sym_t sym;
    if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        printf("Symbol '%s' not found\n", name);
        return -1;
    }
    typedef void (*void_fn)(void);
    void_fn f = (void_fn)sym.val;
    UDYNLINK_PREPARE_CALL(p_mod);
    f();
    return 0;
}

static int call_int_func(const udynlink_module_t *p_mod, const char *name) {
    udynlink_sym_t sym;
    if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        printf("Symbol '%s' not found\n", name);
        return -1;
    }
    typedef int (*int_fn)(void);
    int_fn f = (int_fn)sym.val;
    UDYNLINK_PREPARE_CALL(p_mod);
    return f();
}

static int test_mode(udynlink_load_mode_t mode) {
    udynlink_module_t mod;
    int res = 0;

    if (test_load_module(&mod, mod_relocate_module_data, NULL, 0, mode)) {
        printf("Load failed for mode %d\n", (int)mode);
        return 0;
    }
    CHECK_RAM_SIZE(&mod, sizeof(int));

    /* Mutate runtime state. */
    call_void_func(&mod, "bump");
    call_void_func(&mod, "bump");

    /* Save pre-relocate addresses for the mode invariant check. */
    uintptr_t code0 = udynlink_get_symbol_value(&mod, "doub");
    uintptr_t data0 = udynlink_get_symbol_value(&mod, "g");

    /* Verify state before relocate. */
    if (call_int_func(&mod, "get_counter") != 2) { printf("counter!=2 pre\n"); goto exit; }
    if (*(int *)udynlink_get_symbol_value(&mod, "g") != 20) { printf("g!=20 pre\n"); goto exit; }
    if (!call_int_func(&mod, "check_ptrs")) { printf("check_ptrs failed pre\n"); goto exit; }

    /* Relocate to a foreign (host-allocated) buffer. */
    size_t ram = udynlink_get_ram_size(&mod);
    void *buf = udynlink_external_malloc(ram, NULL, 4, 0);
    if (buf == NULL) { printf("malloc foreign failed\n"); goto exit; }
    if (udynlink_relocate_module(&mod, buf, ram) != UDYNLINK_OK) {
        printf("relocate foreign failed\n"); goto exit;
    }

    /* Data preservation. */
    if (call_int_func(&mod, "get_counter") != 2) { printf("counter!=2 post-foreign\n"); goto exit; }
    if (*(int *)udynlink_get_symbol_value(&mod, "g") != 20) { printf("g!=20 post-foreign\n"); goto exit; }

    /* Rebased pointers work. */
    if (!call_int_func(&mod, "check_ptrs")) { printf("check_ptrs failed post-foreign\n"); goto exit; }

    /* EXTERN slot (printf) untouched: test() must still print. */
    call_int_func(&mod, "test");

    /* Continue mutating. */
    call_void_func(&mod, "bump");
    call_void_func(&mod, "bump");
    if (call_int_func(&mod, "get_counter") != 4) { printf("counter!=4 post-foreign\n"); goto exit; }
    if (*(int *)udynlink_get_symbol_value(&mod, "g") != 30) { printf("g!=30 post-foreign\n"); goto exit; }

    /* Mode invariant. */
    uintptr_t code1 = udynlink_get_symbol_value(&mod, "doub");
    uintptr_t data1 = udynlink_get_symbol_value(&mod, "g");
    if (mode == UDYNLINK_LOAD_MODE_XIP) {
        if (code1 != code0) { printf("XIP code moved (should not)\n"); goto exit; }
        if (data1 == data0) { printf("XIP data did not move\n"); goto exit; }
    } else {
        if (code1 == code0) { printf("code did not move in mode %d\n", (int)mode); goto exit; }
        if (data1 == data0) { printf("data did not move in mode %d\n", (int)mode); goto exit; }
    }

    /* Relocate again via malloc (new_ram=NULL): loader frees the old (from step1). */
    if (udynlink_relocate_module(&mod, NULL, 0) != UDYNLINK_OK) {
        printf("relocate malloc failed\n"); goto exit;
    }
    if (call_int_func(&mod, "get_counter") != 4) { printf("counter!=4 post-malloc\n"); goto exit; }
    if (*(int *)udynlink_get_symbol_value(&mod, "g") != 30) { printf("g!=30 post-malloc\n"); goto exit; }
    if (!call_int_func(&mod, "check_ptrs")) { printf("check_ptrs failed post-malloc\n"); goto exit; }
    call_int_func(&mod, "test");
    call_void_func(&mod, "bump");
    call_void_func(&mod, "bump");
    if (call_int_func(&mod, "get_counter") != 6) { printf("counter!=6 post-malloc\n"); goto exit; }

    /* Free the foreign buffer from the first relocate (now detached from mod). */
    udynlink_external_free(buf, NULL, 4, 0);

    res = 1;
exit:
    test_unload_module(&mod);
    return res;
}

int test_qemu(void) {
    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (!test_mode((udynlink_load_mode_t)i))
            return 0;
    }
    return 1;
}
