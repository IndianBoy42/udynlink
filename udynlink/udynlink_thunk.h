/* Thunk pool for udynlink — runtime-generated ARM call thunks.
 *
 * Provides a bump-allocated pool in executable RAM for gateway + stub
 * thunks that handle r9 (LOT base) switching around module function
 * calls.  This is an optional layer; hosts that do not use it pay zero
 * code/RAM cost.
 *
 * Thunk design: two-level dispatch with per-module gateways (18 bytes)
 * and per-function stubs (10 bytes).  The stub loads the target
 * function address into R12 (IP) via movw+movt, then branches to the
 * module's shared gateway which switches R9 and calls the function.
 * This preserves R0-R3 (argument registers) and saves ~14 bytes per
 * cross-module function reference compared to inline thunks.
 *
 * The pool can be used standalone (without udynlink_deps) via
 * udynlink_thunk_make_call(), which creates a callable function pointer
 * for any exported module symbol.  The resulting pointer requires no r9
 * management from the caller, making it suitable for passing to
 * callbacks, ISRs, or other consumers unaware of the r9/LOT convention.
 *
 * Copyright (c) 2026 udynlink contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __UDYNLINK_THUNK_H__
#define __UDYNLINK_THUNK_H__

#include "udynlink.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Thunk sizes in bytes. */
#define UDYNLINK_GATEWAY_SIZE           18
#define UDYNLINK_STUB_SIZE              10

/* ─── Thunk pool ───────────────────────────────────────────────────── */

/**
 * @brief RAM pool for call thunks.
 *
 * The host provides a contiguous RAM buffer.  Per-module gateways
 * (18 bytes) are allocated from the END of the pool, growing
 * downward.  Per-function stubs (10 bytes) are allocated from the
 * START of the pool, growing upward.  This separation allows
 * udynlink_external_find_stub() to scan only stubs by stepping
 * through the lower region at STUB_SIZE intervals.
 *
 * Pool layout:
 *   [stub1][stub2]...[free gap]...[gateway2][gateway1]
 *   ^                   ^                       ^
 *   base               used                  gateway_top
 *
 * The pool is full when used >= gateway_top.
 *
 * Total per module with N function refs:
 *   18 + 10*N bytes.
 */
typedef struct {
    /** Base of the thunk RAM region (host-provided). */
    uint8_t *base;
    /** Total size of the region in bytes. */
    size_t size;
    /** Stubs: next free offset from base (grows upward). */
    size_t used;
    /** Gateways: next free offset from base (grows downward). */
    size_t gateway_top;
} udynlink_thunk_pool_t;

/**
 * @brief Initialise a thunk pool.
 *
 * @param pool Pointer to the pool structure to initialise.
 * @param buf  Host-provided RAM buffer for thunks.
 * @param sz   Size of @p buf in bytes.
 */
void udynlink_thunk_pool_init(udynlink_thunk_pool_t *pool,
                              uint8_t *buf, size_t sz);

/**
 * @brief Allocate @p n bytes from the thunk pool (stubs region).
 *
 * @param pool Thunk pool to allocate from.
 * @param n    Number of bytes to allocate.
 *
 * @return Pointer to the allocated region, or NULL if the pool is
 *         full.
 */
void *udynlink_thunk_alloc(udynlink_thunk_pool_t *pool, size_t n);

/* ─── Gateway and stub allocation ──────────────────────────────────── */

/**
 * @brief Find an existing gateway for a module by its ram_base value.
 *
 * Scans the gateway region of the thunk pool for a gateway whose
 * embedded ram_base literal matches @p ram_base.
 *
 * @param pool     Thunk pool to search.
 * @param ram_base The module's ram_base value to match.
 *
 * @return Pointer to the matching gateway, or NULL if not found.
 */
uint8_t *udynlink_thunk_find_gateway(const udynlink_thunk_pool_t *pool,
                                      uint32_t ram_base);

/**
 * @brief Allocate a gateway from the top of the thunk pool.
 *
 * @param pool     Thunk pool to allocate from.
 * @param ram_base The module's ram_base value to embed in the gateway.
 *
 * @return Pointer to the allocated gateway, or NULL if the pool is
 *         full.
 */
uint8_t *udynlink_thunk_alloc_gateway(udynlink_thunk_pool_t *pool,
                                       uint32_t ram_base);

/**
 * @brief Allocate a stub and link it to a gateway.
 *
 * @param pool       Thunk pool to allocate from.
 * @param func_addr  Target function address (absolute, as returned by
 *                   udynlink_lookup_symbol).
 * @param gateway    Gateway address the stub will branch to.
 *
 * @return Stub address (with Thumb bit set) on success, 0 on failure
 *         (pool full or branch offset out of ±2 KB range).
 */
uintptr_t udynlink_thunk_alloc_stub(udynlink_thunk_pool_t *pool,
                                     uint32_t func_addr,
                                     const uint8_t *gateway);

/* ─── Byte writers (no pool allocation) ────────────────────────────── */

/**
 * @brief Write an 18-byte gateway at @p dst, embedding @p ram_base.
 *
 * The gateway saves the caller's r9, loads the callee module's ram_base,
 * branches to the function address left in IP (r12) by the stub, and
 * restores r9 on return.
 *
 * @param dst      Destination RAM address (must be writable + executable).
 * @param ram_base The callee module's ram_base to embed in the gateway.
 */
void udynlink_thunk_write_gateway(uint8_t *dst, uint32_t ram_base);

/**
 * @brief Write a 10-byte stub at @p dst and link it to a gateway.
 *
 * The stub loads @p func_addr into IP (r12) via movw+movt and branches to
 * @p gateway.  Use for writing thunks into a caller-owned region (e.g. a
 * module's preallocated .bss thunk pool) instead of the bump-allocated
 * thunk pool.
 *
 * @param dst       Destination RAM address (must be writable + executable).
 * @param func_addr Target function address (absolute).
 * @param gateway   Gateway address the stub will branch to.
 *
 * @return Stub address (with Thumb bit set) on success, 0 on failure
 *         (stub-to-gateway branch offset out of the ±2 KB range).
 */
uintptr_t udynlink_thunk_write_stub(uint8_t *dst, uint32_t func_addr,
                                    const uint8_t *gateway);

/* ─── Stub lookup ─────────────────────────────────────────────────── */

/**
 * @brief Find an existing thunk stub for a function address.
 *
 * Called by udynlink_thunk_make_call() (and optionally by the host)
 * to check whether a stub has already been allocated for a given
 * function address.  The default weak implementation scans the thunk
 * pool linearly; hosts may override with a faster lookup (e.g. hash
 * table) when the pool is large.
 *
 * @param pool      Thunk pool to search.
 * @param func_addr Target function address (absolute, as returned by
 *                  udynlink_lookup_symbol).
 *
 * @return Stub address (with Thumb bit set) if a matching stub exists,
 *         0 otherwise.
 *
 * @note A weak default that scans the pool is provided.  Hosts only
 *       need to override this for performance; correctness is not
 *       affected by the lookup speed.
 */
uintptr_t udynlink_external_find_stub(const udynlink_thunk_pool_t *pool,
                                       uint32_t func_addr);

/* ─── Convenience: create a call thunk for a module symbol ────────── */

/**
 * @brief Create a callable thunk for a module's exported symbol.
 *
 * Looks up @p sym_name in @p p_mod, allocates a gateway (if one does
 * not already exist for this module) and a stub in the thunk pool,
 * and returns a function pointer that can be called directly without
 * any r9 management (UDYNLINK_PREPARE_CALL, UDYNLINK_CALL, etc.).
 *
 * This makes module functions safe to pass as callbacks to ISRs,
 * third-party libraries, or any consumer that is unaware of udynlink's
 * r9/LOT convention.
 *
 * @param pool     Thunk pool to allocate from.
 * @param p_mod    Pointer to the loaded module.
 * @param sym_name Null-terminated symbol name to create a thunk for.
 *
 * @return Stub address (with Thumb bit set) on success, 0 on failure.
 *         Fails if the symbol is not found, is a data symbol, the pool
 *         is full, or the stub-to-gateway branch offset exceeds ±2 KB.
 */
uintptr_t udynlink_thunk_make_call(udynlink_thunk_pool_t *pool,
                                   const udynlink_module_t *p_mod,
                                   const char *sym_name);

#ifdef __cplusplus
}
#endif

#endif /* __UDYNLINK_THUNK_H__ */
