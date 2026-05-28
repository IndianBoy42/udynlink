/* Standalone dependency system for udynlink — implementation.
 *
 * See udynlink_deps.h for API documentation.
 *
 * Copyright (c) 2026 udynlink contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "udynlink_deps.h"
#include "udynlink_externals.h"
#include <string.h>

/* ─── Thunk templates (flash-resident byte arrays) ─────────────────── */

/*
 * Inline per-function thunk (28 bytes, v7m/v8m):
 *
 *   push    {r4, lr}             ; save caller's r4 and return addr
 *   mov     r4, r9               ; save caller's r9
 *   ldr     r2, [pc, #12]        ; load ram_base from literal at +20
 *   mov     r9, r2               ; set callee's r9
 *   ldr.w   ip, [pc, #12]        ; load func_addr from literal at +24
 *   blx     ip                   ; call the target function
 *   mov     r9, r4               ; restore caller's r9
 *   pop     {r4, pc}             ; restore r4, return to caller
 *   nop                          ; alignment padding
 *   .word   ram_base             ; callee module's ram_base
 *   .word   func_addr            ; target function address
 *
 * R12 (IP) is the intra-procedure-call scratch register per AAPCS.
 * Using it for the function address avoids clobbering r0-r3 (args).
 *
 * Verified by hand-encoding and disassembling with
 * arm-none-eabi-objdump -d -M force-thumb.
 */
#define THUNK_RAM_BASE_OFF  20
#define THUNK_FUNC_ADDR_OFF 24

static const uint8_t thunk_template[] = {
    0x10, 0xB5,                       /* push  {r4, lr}            */
    0x4C, 0x46,                       /* mov   r4, r9              */
    0x03, 0x4A,                       /* ldr   r2, [pc, #12]       */
    0x91, 0x46,                       /* mov   r9, r2              */
    0xDF, 0xF8, 0x0C, 0xC0,          /* ldr.w ip, [pc, #12]       */
    0xE0, 0x47,                       /* blx   ip                  */
    0xA1, 0x46,                       /* mov   r9, r4              */
    0x10, 0xBD,                       /* pop   {r4, pc}            */
    0x00, 0xBF,                       /* nop                        */
    0x00, 0x00, 0x00, 0x00,           /* .word ram_base            */
    0x00, 0x00, 0x00, 0x00            /* .word func_addr           */
};

#define THUNK_SIZE sizeof(thunk_template)   /* 28 */

/* ─── Thunk pool ───────────────────────────────────────────────────── */

void udynlink_thunk_pool_init(udynlink_thunk_pool_t *pool,
                              uint8_t *buf, size_t sz) {
    pool->base = buf;
    pool->size = sz;
    pool->used = 0;
}

void *udynlink_thunk_alloc(udynlink_thunk_pool_t *pool, size_t n) {
    if (pool->used + n > pool->size) {
        return NULL;
    }
    void *p = pool->base + pool->used;
    pool->used += n;
    return p;
}

/* ─── Internal: allocate and patch a thunk for a cross-module call ── */

static uintptr_t alloc_thunk(udynlink_thunk_pool_t *pool,
                              uint32_t ram_base, uint32_t func_addr) {
    uint8_t *t = (uint8_t *)udynlink_thunk_alloc(pool, THUNK_SIZE);
    if (t == NULL) return 0;
    memcpy(t, thunk_template, THUNK_SIZE);
    memcpy(t + THUNK_RAM_BASE_OFF, &ram_base, sizeof(uint32_t));
    memcpy(t + THUNK_FUNC_ADDR_OFF, &func_addr, sizeof(uint32_t));
    return (uintptr_t)t | 1;
}

/* ─── Dependency manager ──────────────────────────────────────────── */

void udynlink_dep_mgr_init(udynlink_dep_mgr_t *mgr,
                           udynlink_module_t **buf, size_t cap) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->modules = buf;
    mgr->capacity = cap;
}

/* ─── Dependency query helpers ──────────────────────────────────────── */

int udynlink_dep_is_dependency(const char *name) {
    if (name == NULL) return 0;
    return strncmp(name, UDYNLINK_DEP_PREFIX, UDYNLINK_DEP_PREFIX_LEN) == 0;
}

const char *udynlink_dep_get_name(const char *name) {
    if (!udynlink_dep_is_dependency(name)) return NULL;
    return name + UDYNLINK_DEP_PREFIX_LEN;
}

udynlink_module_t *udynlink_dep_find(udynlink_dep_mgr_t *mgr,
                                     const char *name) {
    if (mgr == NULL || name == NULL) return NULL;
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->modules[i] == NULL) continue;
        const char *mod_name = udynlink_get_module_name(mgr->modules[i]);
        if (mod_name != NULL && strcmp(mod_name, name) == 0) {
            return mgr->modules[i];
        }
    }
    return NULL;
}

/* ─── Circular dependency detection ────────────────────────────────── */

static int is_loading(udynlink_dep_mgr_t *mgr, const char *name) {
    for (size_t i = 0; i < mgr->loading_depth; i++) {
        if (strcmp(mgr->loading_stack[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void push_loading(udynlink_dep_mgr_t *mgr, const char *name) {
    if (mgr->loading_depth < UDYNLINK_DEP_MAX_DEPTH) {
        mgr->loading_stack[mgr->loading_depth++] = name;
    }
}

static void pop_loading(udynlink_dep_mgr_t *mgr) {
    if (mgr->loading_depth > 0) {
        mgr->loading_depth--;
    }
}

/* ─── Module registration ──────────────────────────────────────────── */

static int register_module(udynlink_dep_mgr_t *mgr, udynlink_module_t *p_mod) {
    if (mgr->count >= mgr->capacity) return -1;
    mgr->modules[mgr->count++] = p_mod;
    return 0;
}

static void unregister_module(udynlink_dep_mgr_t *mgr, udynlink_module_t *p_mod) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->modules[i] == p_mod) {
            mgr->modules[i] = mgr->modules[--mgr->count];
            return;
        }
    }
}

/* ─── Resolution helpers ────────────────────────────────────────────── */

uintptr_t udynlink_dep_resolve_dependency(udynlink_dep_mgr_t *mgr,
                                           const char *name) {
    const char *dep_name = udynlink_dep_get_name(name);
    if (dep_name == NULL) return 0;

    if (is_loading(mgr, dep_name)) {
        return UDYNLINK_SYM_DEFERRED;
    }

    udynlink_module_t *p_mod = udynlink_dep_find(mgr, dep_name);
    if (p_mod != NULL) {
        return (uintptr_t)p_mod;
    }

    return 0;
}

void udynlink_dep_register(udynlink_dep_mgr_t *mgr,
                           udynlink_module_t *p_mod) {
    register_module(mgr, p_mod);
}

uintptr_t udynlink_dep_resolve_func(udynlink_dep_mgr_t *mgr,
                                    udynlink_thunk_pool_t *pool,
                                    const char *name) {
    if (mgr == NULL || pool == NULL || name == NULL) return 0;

    for (size_t i = 0; i < mgr->count; i++) {
        udynlink_module_t *p_mod = mgr->modules[i];
        if (p_mod == NULL) continue;

        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) continue;

        if (sym.location == UDYNLINK_SYM_LOCATION_DATA) continue;

        uint32_t ram_base = (uint32_t)p_mod->ram_base;
        uint32_t func_addr = (uint32_t)sym.val;

        return alloc_thunk(pool, ram_base, func_addr);
    }

    return 0;
}

uintptr_t udynlink_dep_resolve_data(udynlink_dep_mgr_t *mgr,
                                    const char *name) {
    if (mgr == NULL || name == NULL) return 0;

    for (size_t i = 0; i < mgr->count; i++) {
        udynlink_module_t *p_mod = mgr->modules[i];
        if (p_mod == NULL) continue;

        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) continue;

        if (sym.location == UDYNLINK_SYM_LOCATION_DATA) {
            return sym.val;
        }
    }

    return 0;
}

/* ─── Module load / unload with dependency tracking ────────────────── */

udynlink_error_t udynlink_dep_load(udynlink_dep_mgr_t *mgr,
                                   udynlink_module_t *p_mod,
                                   const void *base_addr,
                                   void *load_addr, size_t load_size,
                                   udynlink_load_mode_t load_mode,
                                   udynlink_thunk_pool_t *pool) {
    if (mgr == NULL || p_mod == NULL || base_addr == NULL) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }

    const char *mod_name = udynlink_get_module_name_from_image(base_addr);
    if (mod_name != NULL) {
        if (is_loading(mgr, mod_name)) {
            return UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
        }
        push_loading(mgr, mod_name);
    }

    udynlink_error_t err = udynlink_load_module(p_mod, base_addr,
        load_addr, load_size, load_mode);
    if (err != UDYNLINK_OK) {
        if (mod_name != NULL) pop_loading(mgr);
        return err;
    }

    register_module(mgr, p_mod);

    if (mod_name != NULL) pop_loading(mgr);

    (void)pool;
    return UDYNLINK_OK;
}

udynlink_error_t udynlink_dep_unload(udynlink_dep_mgr_t *mgr,
                                     udynlink_module_t *p_mod) {
    if (mgr == NULL || p_mod == NULL) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }

    unregister_module(mgr, p_mod);
    return udynlink_unload_module(p_mod);
}
