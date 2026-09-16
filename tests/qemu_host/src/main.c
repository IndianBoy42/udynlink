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

/* Secondary placement pool backing every non-main (tagged) module section.
 *
 * Every platform's linker script declares the .altpool output section as
 * NOLOAD inside RAM (mps2_an386 routes it to BRAM @ 0x20000000, the board's
 * second region) and every platform therefore reserves it, sized for the
 * sectioned test modules only. It is deliberately small: it is static RAM on
 * boards with as little as 8 KB (stm32f051), and the tests that use it tag a
 * 64-byte buffer and two small functions.
 *
 * Bump allocator with reset-on-empty: exactly one sectioned module lives at
 * a time in the QEMU tests, so freeing the last outstanding sectioned
 * allocation rewinds the frontier — sequential load modes (COPY_ALL,
 * COPY_TEXT_DATA, XIP) each start from a fresh pool. The alloc/free counters
 * let tests assert that unload releases exactly what load allocated. */
#define ALT_POOL_SIZE (4 * 1024)
static uint8_t g_alt_pool[ALT_POOL_SIZE]
    __attribute__((section(".altpool"), used, aligned(32), nocommon));
static size_t g_alt_pool_used;
static size_t g_alt_pool_outstanding;
static size_t g_alt_pool_allocs;
static size_t g_alt_pool_frees;

/* Introspection for tests exercising multi-region placement. */
size_t test_altpool_alloc_count(void) { return g_alt_pool_allocs; }
size_t test_altpool_free_count(void)  { return g_alt_pool_frees; }
const void *test_altpool_base(void)   { return g_alt_pool; }
size_t test_altpool_size(void)        { return ALT_POOL_SIZE; }

/* Any named section routes to the secondary pool; NULL is the module's
 * main block and keeps the plain heap path. */
static int is_secondary_section(const char *section) {
    return section != NULL;
}

/* Failure-injection knobs for the placement-validation paths: the next
 * sectioned allocation returns a deliberately misaligned pointer (the
 * loader must reject the load with UDYNLINK_ERR_LOAD_SECTION_UNALIGNED) or
 * NULL (the loader must reject with UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED). */
static int g_alt_pool_misalign_next;
static int g_alt_pool_fail_next;
void test_altpool_misalign_next(void) { g_alt_pool_misalign_next = 1; }
void test_altpool_fail_next(void)     { g_alt_pool_fail_next = 1; }

void *udynlink_external_malloc(size_t size, const char *section, size_t align, uint32_t flags) {
    if (is_secondary_section(section)) {
        if (g_alt_pool_fail_next) {
            g_alt_pool_fail_next = 0;
            return NULL;
        }
        if (g_alt_pool_misalign_next) {
            g_alt_pool_misalign_next = 0;
            /* Hand back a 4-aligned-but-not-declared-aligned block: checks
             * the loader's per-section alignment validation, not malloc. */
            return (void *)(g_alt_pool + 4);
        }
        if (align < 4u)
            align = 4u;
        uintptr_t base = (uintptr_t)g_alt_pool;
        uintptr_t p = (base + g_alt_pool_used + (align - 1u)) & ~(uintptr_t)(align - 1u);
        size_t off = (size_t)(p - base);
        if (size > ALT_POOL_SIZE - off)
            return NULL;
        g_alt_pool_used = off + size;
        g_alt_pool_outstanding++;
        g_alt_pool_allocs++;
        return (void *)p;
    }
    (void)align;
    return malloc(size);
}

void udynlink_external_free(void *p, const char *section, size_t align, uint32_t flags) {
    (void)align;
    (void)flags;
    if (is_secondary_section(section)) {
        if (p != NULL) {
            g_alt_pool_frees++;
            if (g_alt_pool_outstanding > 0 && --g_alt_pool_outstanding == 0)
                g_alt_pool_used = 0;
        }
        return;
    }
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

uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod, const char *name) {
    (void)p_mod;
    if (is_deferred_symbol(name))
        return UDYNLINK_SYM_DEFERRED;
    if (!strcmp(name, "printf"))
        return (uintptr_t)&printf;
    else if (!strcmp(name, "_write"))
        return (uintptr_t)&_write;
    else if (!strcmp(name, "puts"))
        return (uintptr_t)&puts;
    else if (!strcmp(name, "udynlink_external_malloc"))
        return (uintptr_t)&udynlink_external_malloc;
    else if (!strcmp(name, "udynlink_external_free"))
        return (uintptr_t)&udynlink_external_free;
    else if (!strcmp(name, "udynlink_external_resolve_symbol"))
        return (uintptr_t)&udynlink_external_resolve_symbol;
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
