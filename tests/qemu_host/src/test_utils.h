#ifndef __TEST_UTILS_H__
#define __TEST_UTILS_H__

#include "udynlink.h"

#define CHECK_RAM_SIZE(p, s)\
do {\
    if ((p)->p_header->data_size + (p)->p_header->bss_size < s) {\
        printf("Unexpected RAM size '%u', expected '%u'\n", (p)->p_header->data_size + (p)->p_header->bss_size, s);\
        goto exit;\
    }\
} while(0)

int is_exported_symbol(const udynlink_module_t *p_mod, const char *name);
int is_extern_symbol(const udynlink_module_t *p_mod, const char *name);
int is_weak_symbol(const udynlink_module_t *p_mod, const char *name);
int check_exported_symbols(const udynlink_module_t *p_mod, const char *slist[]);
int check_extern_symbols(const udynlink_module_t *p_mod, const char *slist[]);
int check_weak_symbols(const udynlink_module_t *p_mod, const char *slist[]);
int run_test_func(const udynlink_module_t *p_mod);
udynlink_error_t test_load_module(udynlink_module_t *p_mod, const void *base_addr, void *load_addr, uint32_t load_size, udynlink_load_mode_t load_mode);
udynlink_error_t test_unload_module(udynlink_module_t *p_mod);

void udynlink_test_add_loading_name(const char *name);
void udynlink_test_clear_loading_names(void);
void udynlink_test_defer_symbol(const char *name);
void udynlink_test_clear_deferred_symbols(void);

#endif

