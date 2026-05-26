#include "udynlink.h"
#include "mod_hello_cpp_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int run_test_func2(const udynlink_module_t *p_mod, int a, int b, int c) {
    udynlink_sym_t sym;

    uint32_t* mod_base = (uint32_t*)0x20000000; //see asm_template.tmpl
    *mod_base = p_mod->ram_base;

    // Run test
    if (udynlink_lookup_symbol(p_mod, "test2", &sym) == NULL) {
        printf("'test2' symbol not found.\n");
        return 0;
    }
    int (*p_func)(int, int, int) = (int (*)(int, int, int))sym.val;
    return p_func(a, b, c);
}

int test_qemu(void) {
    const char *exported_syms[] = {"hello", "test", NULL};
    const char *extern_syms[] = {"printf", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL; i <= (int)UDYNLINK_LOAD_MODE_XIP; i ++) {
        if (udynlink_load_module(&mod, mod_hello_cpp_module_data, NULL, 0, (udynlink_load_mode_t)i))
            return 0;

        udynlink_cpp_init(&mod);
        
        CHECK_RAM_SIZE(&mod, 0);
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (!check_extern_symbols(&mod, extern_syms))
            goto exit;
        if (!run_test_func(&mod))
            goto exit;
        if (!run_test_func2(&mod, 1, 2, 3))
            goto exit;

        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
