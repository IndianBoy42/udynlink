/* Thunk pool for udynlink — implementation.
 *
 * See udynlink_thunk.h for API documentation.
 *
 * Copyright (c) 2026 udynlink contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "udynlink_thunk.h"
#include "udynlink_externals.h"
#include <string.h>

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
    if (pool == NULL || pool->used < UDYNLINK_STUB_SIZE) return 0;

    const uint8_t *base = pool->base;
    size_t pos = pool->used;

    while (pos >= UDYNLINK_STUB_SIZE) {
        pos -= UDYNLINK_STUB_SIZE;
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
 *   movw    r9, #ram_lo16      ; load ram_base, low half
 *   movt    r9, #ram_hi16      ; load ram_base, high half
 *   blx     ip                  ; call function (address in ip from stub)
 *   pop.w   {r9, pc}            ; restore r9, return to caller
 *
 * The callee runs with r9 = its own ram_base (GOT base) and callers get theirs
 * back on return.  The constant rides in the instruction stream rather than a
 * PC-relative literal, which would need the gateway to be word aligned: LDR
 * (literal) rounds PC down to a multiple of 4, so a caller that lands gateways
 * on a 2-mod-4 grid (pb_codec does) silently reads garbage into r9.
 *
 * Stub (10 bytes, one per function reference):
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
 * Total per module with N function refs:
 *   18 + 10*N bytes  (vs 28*N bytes with inline thunks)
 *
 * Break-even at N=2:  38 vs 56 bytes.  For N=5:  68 vs 140 bytes.
 */

/* Gateway template: push.w {r9,lr}; movw r9,#lo; movt r9,#hi;
 *                    blx ip; pop.w {r9,pc}.
 *
 * ram_base is loaded with an instruction pair, not a PC-relative literal: a
 * literal load rounds PC down to a multiple of 4, so its address depends on the
 * gateway's own alignment — which the pool's callers control (pb_codec carves
 * its gateway region out of a byte array, landing on a 2-mod-4 grid).  Encoding
 * the constant in the instruction stream works at any halfword-aligned address.
 */
static const uint8_t gateway_template[UDYNLINK_GATEWAY_SIZE] = {
    0x2D, 0xE9, 0x00, 0x42,       /* push.w  {r9, lr}          */
    0x40, 0xF2, 0x00, 0x09,       /* movw    r9, #ram_lo16     */
    0xC0, 0xF2, 0x00, 0x09,       /* movt    r9, #ram_hi16     */
    0xE0, 0x47,                   /* blx     ip                */
    0xBD, 0xE8, 0x00, 0x82,       /* pop.w   {r9, pc}          */
};

/* Byte offsets of the movw/movt immediates inside the gateway (see above). */
#define GATEWAY_MOVW_OFF      4
#define GATEWAY_MOVT_OFF      8

/* ─── movw / movt helpers ──────────────────────────────────────────── */

// MOVW/MOVT (Thumb-2, immediate): `1111 0i10 0100 imm4 | 0 imm3 Rd imm8`.
static void encode_mov_imm(uint8_t *dst, unsigned rd, uint16_t imm16, int top) {
    unsigned i    = (imm16 >> 11) & 1;
    unsigned imm4 = (imm16 >> 12) & 0xF;
    unsigned imm3 = (imm16 >>  8) & 0x7;
    unsigned imm8 =  imm16        & 0xFF;
    uint16_t hw0  = (top ? 0xF2C0 : 0xF240) | (i << 10) | imm4;
    uint16_t hw1  = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
    dst[0] = hw0 & 0xFF; dst[1] = hw0 >> 8;
    dst[2] = hw1 & 0xFF; dst[3] = hw1 >> 8;
}

static void encode_movw_ip(uint8_t *dst, uint16_t imm16) {
    encode_mov_imm(dst, 12, imm16, 0);
}

static void encode_movt_ip(uint8_t *dst, uint16_t imm16) {
    encode_mov_imm(dst, 12, imm16, 1);
}

// Read back a MOVW/MOVT immediate written by encode_mov_imm.
static uint16_t decode_mov_imm(const uint8_t *src) {
    uint16_t hw0 = (uint16_t)(src[0] | (src[1] << 8));
    uint16_t hw1 = (uint16_t)(src[2] | (src[3] << 8));
    unsigned i    = (hw0 >> 10) & 1;
    unsigned imm4 = hw0 & 0xF;
    unsigned imm3 = (hw1 >> 12) & 0x7;
    unsigned imm8 = hw1 & 0xFF;
    return (uint16_t)((i << 11) | (imm4 << 12) | (imm3 << 8) | imm8);
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
    /* Hand out word-aligned gateways: 32-bit Thumb instructions need only
     * halfword alignment, but a word-aligned grid keeps every template offset
     * valid whatever the templates grow into.  Align the base up and the top
     * down; the few bytes lost are irrelevant next to the surprise of a
     * 2-mod-4 gateway. */
    size_t misalign = (size_t)((uintptr_t)pool->base & 3u);
    if (misalign != 0) {
        size_t skip = 4u - misalign;
        if (skip >= pool->size) {
            pool->base += pool->size;
            pool->size = 0;
        } else {
            pool->base += skip;
            pool->size -= skip;
        }
    }
    pool->size &= ~(size_t)3u;
    pool->used = 0;
    pool->gateway_top = pool->size;
}

void *udynlink_thunk_alloc(udynlink_thunk_pool_t *pool, size_t n) {
    if (pool->used + n > pool->gateway_top) {
        return NULL;
    }
    void *p = pool->base + pool->used;
    pool->used += n;
    return p;
}

/* ─── Byte writers (no pool allocation) ────────────────────────────── */

void udynlink_thunk_write_gateway(uint8_t *dst, uint32_t ram_base) {
    memcpy(dst, gateway_template, UDYNLINK_GATEWAY_SIZE);
    encode_mov_imm(dst + GATEWAY_MOVW_OFF, 9, (uint16_t)(ram_base & 0xFFFF), 0);
    encode_mov_imm(dst + GATEWAY_MOVT_OFF, 9, (uint16_t)(ram_base >> 16), 1);
}

uintptr_t udynlink_thunk_write_stub(uint8_t *dst, uint32_t func_addr,
                                    const uint8_t *gateway_addr) {
    encode_movw_ip(dst, (uint16_t)(func_addr & 0xFFFF));
    encode_movt_ip(dst + 4, (uint16_t)((func_addr >> 16) & 0xFFFF));

    /* b.n from stub's offset+8 to gateway.
     * The b.n is at dst+8.  PC = dst+8+4 = dst+12.
     * offset = gateway_addr - (dst + 12), in halfwords. */
    intptr_t byte_offset = (intptr_t)gateway_addr - (intptr_t)(dst + 12);
    int16_t hw_offset = (int16_t)(byte_offset / 2);

    if (hw_offset < -1024 || hw_offset > 1023) {
        return 0;
    }

    encode_b_n(dst + 8, hw_offset);
    return (uintptr_t)dst | 1;
}

/* ─── Gateway / stub allocation ────────────────────────────────────── */

uint8_t *udynlink_thunk_find_gateway(const udynlink_thunk_pool_t *pool,
                                      uint32_t ram_base) {
    if (pool == NULL) return NULL;
    for (size_t off = pool->gateway_top; off + UDYNLINK_GATEWAY_SIZE <= pool->size;
         off += UDYNLINK_GATEWAY_SIZE) {
        const uint8_t *g = pool->base + off;
        uint32_t g_ram_base;
        g_ram_base = (uint32_t)decode_mov_imm(g + GATEWAY_MOVW_OFF) |
                     ((uint32_t)decode_mov_imm(g + GATEWAY_MOVT_OFF) << 16);
        if (g_ram_base == ram_base) {
            return (uint8_t *)g;
        }
    }
    return NULL;
}

uint8_t *udynlink_thunk_alloc_gateway(udynlink_thunk_pool_t *pool,
                                       uint32_t ram_base) {
    if (pool->gateway_top < pool->used + UDYNLINK_GATEWAY_SIZE) {
        return NULL;
    }
    pool->gateway_top -= UDYNLINK_GATEWAY_SIZE;
    uint8_t *g = pool->base + pool->gateway_top;
    udynlink_thunk_write_gateway(g, ram_base);
    return g;
}

uintptr_t udynlink_thunk_alloc_stub(udynlink_thunk_pool_t *pool,
                                     uint32_t func_addr,
                                     const uint8_t *gateway_addr) {
    uint8_t *s = (uint8_t *)udynlink_thunk_alloc(pool, UDYNLINK_STUB_SIZE);
    if (s == NULL) return 0;

    if (udynlink_thunk_write_stub(s, func_addr, gateway_addr) == 0) {
        pool->used -= UDYNLINK_STUB_SIZE;
        return 0;
    }

    return (uintptr_t)s | 1;
}

/* ─── Convenience: create a call thunk for a module symbol ────────── */

uintptr_t udynlink_thunk_make_call(udynlink_thunk_pool_t *pool,
                                   const udynlink_module_t *p_mod,
                                   const char *sym_name) {
    if (pool == NULL || p_mod == NULL || sym_name == NULL) return 0;

    udynlink_sym_t sym;
    if (udynlink_lookup_symbol(p_mod, sym_name, &sym) == NULL) return 0;
    if (sym.location == UDYNLINK_SYM_LOCATION_DATA) return 0;

    uint32_t ram_base = (uint32_t)p_mod->ram_base;
    uint32_t func_addr = (uint32_t)sym.val;

    uintptr_t existing = udynlink_external_find_stub(pool, func_addr);
    if (existing != 0) return existing;

    uint8_t *gateway = udynlink_thunk_find_gateway(pool, ram_base);
    if (gateway == NULL) {
        gateway = udynlink_thunk_alloc_gateway(pool, ram_base);
        if (gateway == NULL) return 0;
    }

    return udynlink_thunk_alloc_stub(pool, func_addr, gateway);
}
