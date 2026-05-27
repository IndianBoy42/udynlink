#include "udynlink.h"
#include <stdio.h>

extern void udynlink_test_register_loading(const void *base_addr);
extern void udynlink_test_unregister_loading(const void *base_addr);
extern void udynlink_test_register_module(udynlink_module_t *p_mod);
extern void udynlink_test_unregister_module(udynlink_module_t *p_mod);

int is_exported_symbol(const udynlink_module_t *p_mod, const char *name) {
    udynlink_sym_t sym;

    if(udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        return 0;
    }
    return sym.type == UDYNLINK_SYM_TYPE_EXPORTED;
}

int is_extern_symbol(const udynlink_module_t *p_mod, const char *name) {
    udynlink_sym_t sym;

    if(udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        return 0;
    }
    return sym.type == UDYNLINK_SYM_TYPE_EXTERN;
}

int is_weak_symbol(const udynlink_module_t *p_mod, const char *name) {
    udynlink_sym_t sym;

    if(udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        return 0;
    }
    return sym.type == UDYNLINK_SYM_TYPE_WEAK;
}

int check_exported_symbols(const udynlink_module_t *p_mod, const char *slist[]) {
    for (; *slist; slist ++) {
        if (!is_exported_symbol(p_mod, *slist)) {
            printf("Exported symbol '%s' not found in symbol table.\n", *slist);
            return 0;
        }
    }
    return 1;
}

int check_extern_symbols(const udynlink_module_t *p_mod, const char *slist[]) {
    for (; *slist; slist ++) {
        if (!is_extern_symbol(p_mod, *slist)) {
            printf("Extern symbol '%s' not found in symbol table.\n", *slist);
            return 0;
        }
    }
    return 1;
}

int check_weak_symbols(const udynlink_module_t *p_mod, const char *slist[]) {
    for (; *slist; slist ++) {
        if (!is_weak_symbol(p_mod, *slist)) {
            printf("Weak symbol '%s' not found in symbol table.\n", *slist);
            return 0;
        }
    }
    return 1;
}

int run_test_func(const udynlink_module_t *p_mod) {
    udynlink_sym_t sym;

    // Run test
    if (udynlink_lookup_symbol(p_mod, "test", &sym) == NULL) {
        printf("'test' symbol not found.\n");
        return 0;
    }
    int (*p_func)(void) = (int (*)(void))sym.val;
    UDYNLINK_PREPARE_CALL(p_mod);
    return p_func();
}

udynlink_error_t test_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, size_t load_size, udynlink_load_mode_t load_mode) {
    udynlink_test_register_loading(base_addr);
    udynlink_error_t err = udynlink_load_module(p_mod, base_addr, load_addr, load_size, load_mode);
    if (err == UDYNLINK_OK) {
        udynlink_test_unregister_loading(base_addr);
        udynlink_test_register_module(p_mod);
    } else {
        udynlink_test_unregister_loading(base_addr);
    }
    return err;
}

udynlink_error_t test_unload_module(udynlink_module_t *p_mod) {
    udynlink_test_unregister_module(p_mod);
    return udynlink_unload_module(p_mod);
}

