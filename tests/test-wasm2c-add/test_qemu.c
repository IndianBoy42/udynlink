#include "udynlink.h"
#include "mod_wasm2c_add_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    const char *exported_syms[] = {"add", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_wasm2c_add_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        {
            udynlink_sym_t sym;
            if (udynlink_lookup_symbol(&mod, "add", &sym) == NULL)
                goto exit;
            /* mkwasm2c-module generates a wrapper named after the wasm
             * export; the host prepares r9 and calls it as a plain C
             * function. */
            UDYNLINK_PREPARE_CALL(&mod);
            unsigned int (*add)(unsigned int, unsigned int) =
                (unsigned int (*)(unsigned int, unsigned int))sym.val;
            unsigned int sum = add(30, 70);
            printf("add: 30 + 70 = %u\n", sum);
            if (sum != 100)
                goto exit;
        }
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
