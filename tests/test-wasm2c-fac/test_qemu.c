#include "udynlink.h"
#include "mod_wasm2c_fac_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    const char *exported_syms[] = {"fac", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_wasm2c_fac_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        {
            udynlink_sym_t sym;
            if (udynlink_lookup_symbol(&mod, "fac", &sym) == NULL)
                goto exit;
            /* Recursive wasm-to-C function: wasm-side depth counting is
             * disabled here (no --stack-depth-limit), so the native stack
             * absorbs the recursion. */
            UDYNLINK_PREPARE_CALL(&mod);
            unsigned int (*fac)(unsigned int) =
                (unsigned int (*)(unsigned int))sym.val;
            unsigned int f = fac(5);
            printf("fac: 5! = %u\n", f);
            if (f != 120)
                goto exit;
        }
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
