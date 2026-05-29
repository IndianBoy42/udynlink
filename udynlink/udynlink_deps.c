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

#define GATEWAY_SIZE  18
#define STUB_SIZE     10

udynlink_module_t *udynlink_external_dep_load(const char *name)
    __attribute__((weak));
udynlink_module_t *udynlink_external_dep_load(const char *name) {
    (void)name;
    return NULL;
}

/* ─── Default find_stub: scan the thunk pool ──────────────────────── */

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int decode_movw_ip_imm16(const uint8_t *stub, uint16_t *out) {
    uint16_t hw0 = read_le16(stub);
    uint16_t hw1 = read_le16(stub + 2);
    if ((hw0 & 0xFBF0) != 0xF240) return 0;
    if ((hw1 & 0x0F00) != 0x0C00) return 0;
    unsigned i    = (hw0 >> 10) & 1;
    unsigned imm4 = hw0 & 0xF;
    unsigned imm3 = (hw1 >> 12) & 0x7;
    unsigned imm8 = hw1 & 0xFF;
    *out = (i << 11) | (imm4 << 12) | (imm3 << 8) | imm8;
    return 1;
}

static int decode_movt_ip_imm16(const uint8_t *stub, uint16_t *out) {
    uint16_t hw0 = read_le16(stub);
    uint16_t hw1 = read_le16(stub + 2);
    if ((hw0 & 0xFBF0) != 0xF2C0) return 0;
    if ((hw1 & 0x0F00) != 0x0C00) return 0;
    unsigned i    = (hw0 >> 10) & 1;
    unsigned imm4 = hw0 & 0xF;
    unsigned imm3 = (hw1 >> 12) & 0x7;
    unsigned imm8 = hw1 & 0xFF;
    *out = (i << 11) | (imm4 << 12) | (imm3 << 8) | imm8;
    return 1;
}

uintptr_t udynlink_external_find_stub(const udynlink_thunk_pool_t *pool,
                                       uint32_t func_addr)
    __attribute__((weak));
uintptr_t udynlink_external_find_stub(const udynlink_thunk_pool_t *pool,
                                       uint32_t func_addr) {
    if (pool == NULL || pool->used < STUB_SIZE) return 0;

    const uint8_t *base = pool->base;
    size_t pos = pool->used;

    while (pos >= STUB_SIZE) {
        pos -= STUB_SIZE;
        const uint8_t *candidate = base + pos;
        uint16_t lo16, hi16;
        if (decode_movw_ip_imm16(candidate, &lo16) &&
            decode_movt_ip_imm16(candidate + 4, &hi16)) {
            uint32_t addr = ((uint32_t)hi16 << 16) | lo16;
            if (addr == func_addr) {
                return (uintptr_t)candidate | 1;
            }
        }
    }
    return 0;
}

/* ─── Thunk templates (flash-resident byte arrays) ─────────────────── */

/*
 * Two-level dispatch: per-module gateway + per-function stubs.
 *
 * Gateway (18 bytes, one per target module):
 *   push.w  {r9, lr}           ; save caller's r9 and return addr
 *   ldr.w   r9, [pc, #4]       ; load ram_base from literal
 *   blx     ip                  ; call function (address in ip from stub)
 *   pop.w   {r9, pc}            ; restore r9, return to caller
 *   .word   ram_base            ; callee module's ram_base
 *
 * Stub (10 bytes, one per cross-module function reference):
 *   movw    ip, #func_lo16      ; load low 16 bits of func_addr
 *   movt    ip, #func_hi16      ; load high 16 bits of func_addr
 *   b.n     gateway             ; branch to module's gateway
 *
 * R12 (IP) is the intra-procedure-call scratch register per AAPCS.
 * Using it for the function address preserves r0-r3 (arguments).
 *
 * The gateway is placed BEFORE its stubs in the thunk pool.
 * Stubs branch backward to the gateway via a Thumb-16 unconditional
 * branch (b.n), patched at allocation time (±2 KB range).
 *
 * Total per module with N cross-module function refs:
 *   18 + 10*N bytes  (vs 28*N bytes with inline thunks)
 *
 * Break-even at N=2:  38 vs 56 bytes.  For N=5:  68 vs 140 bytes.
 */

/* Gateway template: push.w {r9,lr}; ldr.w r9,[pc,#4]; blx ip; pop.w {r9,pc}; .word ram_base */
static const uint8_t gateway_template[GATEWAY_SIZE] = {
    0x2D, 0xE9, 0x00, 0x42,       /* push.w  {r9, lr}          */
    0xDF, 0xF8, 0x04, 0x90,       /* ldr.w   r9, [pc, #4]      */
    0xE0, 0x47,                   /* blx     ip                 */
    0xBD, 0xE8, 0x00, 0x82,       /* pop.w   {r9, pc}           */
    0x00, 0x00, 0x00, 0x00,       /* .word   ram_base           */
};

#define GATEWAY_RAM_BASE_OFF  14

/* ─── movw / movt helpers ──────────────────────────────────────────── */

static void encode_movw_ip(uint8_t *dst, uint16_t imm16) {
    unsigned i    = (imm16 >> 11) & 1;
    unsigned imm4 = (imm16 >> 12) & 0xF;
    unsigned imm3 = (imm16 >>  8) & 0x7;
    unsigned imm8 =  imm16        & 0xFF;
    uint16_t hw0  = 0xF240 | (i << 10) | imm4;
    uint16_t hw1  = (imm3 << 12) | (0xC << 8) | imm8;
    dst[0] = hw0 & 0xFF; dst[1] = hw0 >> 8;
    dst[2] = hw1 & 0xFF; dst[3] = hw1 >> 8;
}

static void encode_movt_ip(uint8_t *dst, uint16_t imm16) {
    unsigned i    = (imm16 >> 11) & 1;
    unsigned imm4 = (imm16 >> 12) & 0xF;
    unsigned imm3 = (imm16 >>  8) & 0x7;
    unsigned imm8 =  imm16        & 0xFF;
    uint16_t hw0  = 0xF2C0 | (i << 10) | imm4;
    uint16_t hw1  = (imm3 << 12) | (0xC << 8) | imm8;
    dst[0] = hw0 & 0xFF; dst[1] = hw0 >> 8;
    dst[2] = hw1 & 0xFF; dst[3] = hw1 >> 8;
}

/*
 * Encode a Thumb-16 unconditional branch (b.n).
 * offset_halfwords is signed, relative to PC = instruction_address + 4.
 * Range: ±2048 bytes (±1024 halfwords).
 */
static void encode_b_n(uint8_t *dst, int16_t offset_halfwords) {
    uint16_t hw = 0xE000 | (offset_halfwords & 0x7FF);
    dst[0] = hw & 0xFF; dst[1] = hw >> 8;
}

/* ─── Thunk pool ───────────────────────────────────────────────────── */

void udynlink_thunk_pool_init(udynlink_thunk_pool_t *pool,
                              uint8_t *buf, size_t sz) {
    pool->base = buf;
    pool->size = sz;
    pool->used = 0;
    pool->gateway_top = sz;
}

void *udynlink_thunk_alloc(udynlink_thunk_pool_t *pool, size_t n) {
    if (pool->used + n > pool->gateway_top) {
        return NULL;
    }
    void *p = pool->base + pool->used;
    pool->used += n;
    return p;
}

/* ─── Internal: allocate a gateway from the top of the pool ──────── */

static uint8_t *alloc_gateway(udynlink_thunk_pool_t *pool,
                               uint32_t ram_base) {
    if (pool->gateway_top < pool->used + GATEWAY_SIZE) {
        return NULL;
    }
    pool->gateway_top -= GATEWAY_SIZE;
    uint8_t *g = pool->base + pool->gateway_top;
    memcpy(g, gateway_template, GATEWAY_SIZE);
    memcpy(g + GATEWAY_RAM_BASE_OFF, &ram_base, sizeof(uint32_t));
    return g;
}

/* ─── Internal: allocate a stub and link it to a gateway ──────────── */

static uintptr_t alloc_stub(udynlink_thunk_pool_t *pool,
                             uint32_t func_addr,
                             const uint8_t *gateway_addr) {
    uint8_t *s = (uint8_t *)udynlink_thunk_alloc(pool, STUB_SIZE);
    if (s == NULL) return 0;

    encode_movw_ip(s, (uint16_t)(func_addr & 0xFFFF));
    encode_movt_ip(s + 4, (uint16_t)((func_addr >> 16) & 0xFFFF));

    /* b.n from stub's offset+8 to gateway.
     * The b.n is at s+8.  PC = s+8+4 = s+12.
     * offset = gateway_addr - (s + 12), in halfwords. */
    intptr_t byte_offset = (intptr_t)gateway_addr - (intptr_t)(s + 12);
    int16_t hw_offset = (int16_t)(byte_offset / 2);

    if (hw_offset < -1024 || hw_offset > 1023) {
        pool->used -= STUB_SIZE;
        return 0;
    }

    encode_b_n(s + 8, hw_offset);
    return (uintptr_t)s | 1;
}

/* ─── Dependency manager ──────────────────────────────────────────── */

void udynlink_dep_mgr_init(udynlink_dep_mgr_t *mgr,
                           udynlink_dep_entry_t *buf, size_t cap) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->entries = buf;
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
        if (mgr->entries[i].p_mod == NULL) continue;
        const char *mod_name = udynlink_get_module_name(mgr->entries[i].p_mod);
        if (mod_name != NULL && strcmp(mod_name, name) == 0) {
            return mgr->entries[i].p_mod;
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
    mgr->entries[mgr->count].p_mod = p_mod;
    mgr->entries[mgr->count].gateway = NULL;
    mgr->count++;
    return 0;
}

static void unregister_module(udynlink_dep_mgr_t *mgr, udynlink_module_t *p_mod) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].p_mod == p_mod) {
            mgr->entries[i] = mgr->entries[--mgr->count];
            return;
        }
    }
}

static udynlink_dep_entry_t *find_entry(udynlink_dep_mgr_t *mgr,
                                         const udynlink_module_t *p_mod) {
    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].p_mod == p_mod) {
            return &mgr->entries[i];
        }
    }
    return NULL;
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

    p_mod = udynlink_external_dep_load(dep_name);
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
        udynlink_module_t *p_mod = mgr->entries[i].p_mod;
        if (p_mod == NULL) continue;

        udynlink_sym_t sym;
        if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) continue;

        if (sym.location == UDYNLINK_SYM_LOCATION_DATA) continue;

        uint32_t ram_base = (uint32_t)p_mod->ram_base;
        uint32_t func_addr = (uint32_t)sym.val;

        udynlink_dep_entry_t *entry = &mgr->entries[i];

        uintptr_t existing = udynlink_external_find_stub(pool, func_addr);
        if (existing != 0) return existing;

        if (entry->gateway == NULL) {
            entry->gateway = alloc_gateway(pool, ram_base);
            if (entry->gateway == NULL) return 0;
        }

        return alloc_stub(pool, func_addr, entry->gateway);
    }

    return 0;
}

uintptr_t udynlink_dep_resolve_data(udynlink_dep_mgr_t *mgr,
                                    const char *name) {
    if (mgr == NULL || name == NULL) return 0;

    for (size_t i = 0; i < mgr->count; i++) {
        udynlink_module_t *p_mod = mgr->entries[i].p_mod;
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
