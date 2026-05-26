#include "udynlink.h"
#include "udynlink_externals.h"
#include "mod_globals2_module_data.h"
#include "test_utils.h"
#include <stdio.h>
#include <string.h>

#define INITIAL_EXT_I                       40
#define EXPECTED_EXT_I_VAL                  (-INITIAL_EXT_I)
#define EXPECTED_G_VAL                      (60 + INITIAL_EXT_I)

int ext_i = INITIAL_EXT_I;

uint32_t test_resolve_symbol(const char *name) {
    if (!strcmp(name, "ext_i"))
        return (uint32_t)&ext_i;
    else
        return 0;
}

int test_qemu(void) {
    const char *exported_syms[] = {"test", "g", NULL};
    const char *extern_syms[] = {"ext_i", NULL};
    udynlink_module_t mod;
    udynlink_sym_t sym;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_globals2_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        CHECK_RAM_SIZE(&mod, sizeof(int));
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (!check_extern_symbols(&mod, extern_syms))
            goto exit;
        run_test_func(&mod); // no need to check the result here
        // Check the expected value of the global variable
        uint32_t v = *(int*)udynlink_lookup_symbol(&mod, "g", &sym)->val;
        if (v != EXPECTED_G_VAL) {
            printf("Unexpected value %d for variable 'g', expected %d\n", v, EXPECTED_G_VAL);
            goto exit;
        }
        // Check that the module modified the value of "ext_i" properly
        if (ext_i != EXPECTED_EXT_I_VAL) {
            printf("The module didn't modify 'ext_i' properly: expected %d, got %d\n", EXPECTED_EXT_I_VAL, ext_i);
            goto exit;
        }
        udynlink_unload_module(&mod);
        ext_i = INITIAL_EXT_I;
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
