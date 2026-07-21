#include "udynlink.h"
#include "udynlink_cpp_abi.h"
#include "mod_heap_module_data.h"
#include <stdio.h>

extern "C" uintptr_t test_resolve_symbol(const char *name) {
    return udynlink_cpp_resolve_abi_symbol(name);
}

extern "C" int test_qemu(void) {
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        udynlink_error_t err = udynlink_load_module(&mod, mod_heap_module_data, NULL, 0, (udynlink_load_mode_t)i);
        if (err) {
            printf("load failed mode %d: %s\n", i, udynlink_error_msg(&err));
            return 0;
        }
        udynlink_cpp_init(&mod);

        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(&mod, "test", &sym) == NULL) {
            printf("'test' symbol not found.\n");
            goto exit;
        }
        int (*p_func)(void) = (int (*)(void))sym.val;
        UDYNLINK_PREPARE_CALL(&mod);
        if (!p_func()) {
            printf("test() returned 0\n");
            goto exit;
        }

        printf("test ok mode %d\n", i);
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
