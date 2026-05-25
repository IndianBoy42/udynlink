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

uint32_t test_resolve_symbol(const char *name) __attribute__((weak));
uint32_t test_resolve_symbol(const char *name) {
    (void)name;
    return 0;
}

uint32_t udynlink_external_resolve_critical_symbol(const char *name) __attribute__((weak));
uint32_t udynlink_external_resolve_critical_symbol(const char *name) {
    (void)name;
    return 0;
}

extern int _write(int file, char *ptr, int len);

uint32_t udynlink_external_resolve_symbol(const char *name) {
    if (!strcmp(name, "printf"))
        return (uint32_t)&printf;
    else if (!strcmp(name, "_write"))
        return (uint32_t)&_write;
    else if (!strcmp(name, "puts"))
        return (uint32_t)&puts;
    else
        return test_resolve_symbol(name);
}

udynlink_module_t *udynlink_external_get_module_handle(const char *module_name) {
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i] == NULL) continue;
        const char *mod_name = udynlink_get_module_name(g_modules[i]);
        if (mod_name && !strcmp(mod_name, module_name))
            return g_modules[i];
    }
    return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// Test me!

extern int test_qemu(void);

int main() {
#ifdef UDYNLINK_TEST_DEBUG_LEVEL
    udynlink_set_debug_level(UDYNLINK_TEST_DEBUG_LEVEL);
#else
    udynlink_set_debug_level(UDYNLINK_DEBUG_NONE);
#endif
    int ok = test_qemu();
    printf (ok ? "*** TEST OK ***\n" : "*** TEST FAILED! ***\n");
    exit(ok ? 0 : 1);
}
