//
// This file is part of the GNU ARM Eclipse distribution.
// Copyright (c) 2014 Liviu Ionescu.
//

// ----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "udynlink.h"
#include "udynlink_hash.h"

#ifndef UDYNLINK_MAX_MODULES
#define UDYNLINK_MAX_MODULES 8
#endif

static udynlink_module_t *g_modules[UDYNLINK_MAX_MODULES];
static int g_module_count = 0;

static const void *g_loading_addrs[UDYNLINK_MAX_MODULES];
static int g_loading_count = 0;

static const char *g_deferred_symbols[UDYNLINK_MAX_MODULES];
static int g_deferred_count = 0;

void udynlink_test_register_loading(const void *base_addr) {
    if (g_loading_count < UDYNLINK_MAX_MODULES)
        g_loading_addrs[g_loading_count++] = base_addr;
}

void udynlink_test_unregister_loading(const void *base_addr) {
    for (int i = 0; i < g_loading_count; i++) {
        if (g_loading_addrs[i] == base_addr) {
            g_loading_addrs[i] = g_loading_addrs[--g_loading_count];
            return;
        }
    }
}

void udynlink_test_register_module(udynlink_module_t *p_mod) {
    if (g_module_count < UDYNLINK_MAX_MODULES) {
        g_modules[g_module_count++] = p_mod;
    }
}

void udynlink_test_unregister_module(udynlink_module_t *p_mod) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i] == p_mod) {
            g_modules[i] = g_modules[--g_module_count];
            return;
        }
    }
}

void udynlink_test_defer_symbol(const char *name) {
    if (g_deferred_count < UDYNLINK_MAX_MODULES)
        g_deferred_symbols[g_deferred_count++] = name;
}

void udynlink_test_clear_deferred_symbols(void) {
    g_deferred_count = 0;
}

static int is_deferred_symbol(const char *name) {
    for (int i = 0; i < g_deferred_count; i++) {
        if (g_deferred_symbols[i] && !strcmp(g_deferred_symbols[i], name))
            return 1;
    }
    return 0;
}

///////////////////////////////////////////////////////////////////////////////

void *udynlink_external_malloc(size_t size) {
    return malloc(size);
}

void udynlink_external_free(void *p) {
    free(p);
}

void udynlink_external_vprintf(const char *s, va_list va) {
    vprintf(s, va);
}

uintptr_t test_resolve_symbol(const char *name) __attribute__((weak));
uintptr_t test_resolve_symbol(const char *name) {
    (void)name;
    return 0;
}

extern int _write(int file, char *ptr, int len);

uintptr_t udynlink_external_resolve_symbol(const char *name) {
    if (is_deferred_symbol(name))
        return UDYNLINK_SYM_DEFERRED;
    if (!strcmp(name, "printf"))
        return (uintptr_t)&printf;
    else if (!strcmp(name, "_write"))
        return (uintptr_t)&_write;
    else if (!strcmp(name, "puts"))
        return (uintptr_t)&puts;
    else
        return test_resolve_symbol(name);
}

///////////////////////////////////////////////////////////////////////////////
// Test me!

extern int test_qemu(void);

int main() {
    udynlink_set_debug_level(UDYNLINK_DEBUG_LEVEL);
    int ok = test_qemu();
    printf (ok ? "*** TEST OK ***\n" : "*** TEST FAILED! ***\n");
    exit(ok ? 0 : 1);
}
