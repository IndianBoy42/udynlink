/* ARM Cortex-M micro dynamic linker (udynlink) C convenience layer.
 *
 * Copyright (c) 2026 Bogdan Marinescu
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

#ifndef __UDYNLINK_CALL_H__
#define __UDYNLINK_CALL_H__

#include "udynlink.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reusable function handle for amortized symbol lookups.
 *
 * Resolve once with udynlink_resolve_func(), then call many times
 * via UDYNLINK_CALL().  This avoids the O(N) string search on
 * every invocation.
 */
typedef struct {
    /** Module the symbol belongs to. */
    const udynlink_module_t *p_mod;
    /** Resolved symbol address (already relocated). */
    uintptr_t addr;
    /** Symbol name (kept for debugging / validation). */
    const char *name;
} udynlink_func_t;

/**
 * @brief Resolve a symbol by name into a reusable handle.
 *
 * @param[in]  p_mod Pointer to the loaded module.
 * @param[in]  name  Null-terminated symbol name.
 * @param[out] p_out Handle to populate on success.
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 *         On failure @p p_out is zeroed.
 */
static inline udynlink_error_t udynlink_resolve_func(const udynlink_module_t *p_mod,
                                                     const char *name,
                                                     udynlink_func_t *p_out) {
    udynlink_sym_t sym;
    if (p_out == NULL) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    *p_out = (udynlink_func_t){NULL, 0, NULL};
    if (p_mod == NULL || name == NULL) {
        return UDYNLINK_ERR_INVALID_MODULE;
    }
    if (udynlink_lookup_symbol(p_mod, name, &sym) == NULL) {
        return UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL;
    }
    p_out->p_mod = p_mod;
    p_out->addr = sym.val;
    p_out->name = name;
    return UDYNLINK_OK;
}

/**
 * @brief Directly set the r9 register to the module RAM base.
 *
 * For --no-prologue modules (or when maximum performance is needed),
 * the host can bypass the assembly prologue and load r9 directly.
 *
 * @note This is Cortex-M specific inline assembly.
 */
#define UDYNLINK_SET_R9(p_mod) \
    __asm volatile ("mov r9, %0" :: "r"((uint32_t)(p_mod)->ram_base) : "r9")

/**
 * @brief Call a module function through a reusable handle.
 *
 * Automatically saves the caller's r9, sets r9 to the module's RAM base
 * via UDYNLINK_PREPARE_CALL(), invokes the function, and restores the
 * original r9.  This works safely for both prologued and --no-prologue
 * modules because it always manages r9 explicitly.
 *
 * The cast uses a variadic function-pointer type
 * `ret_type (*)(...)` which GCC accepts for any argument list.
 *
 * @param[in] p_func   Pointer to a resolved udynlink_func_t handle.
 * @param[in] ret_type Return type of the function.
 * @param[in] args     Parenthesized argument list, e.g. (42) or (1, 2).
 *
 * @return The value returned by the module function.
 *
 * Example:
 * @code
 *   udynlink_func_t h;
 *   udynlink_resolve_func(&mod, "add", &h);
 *   int r = UDYNLINK_CALL(&h, int, (1, 2));
 * @endcode
 *
 * @warning Not interrupt-safe if the called function itself is not
 *          re-entrant.  The r9 save/restore happens in the caller's
 *          stack frame, so nested module calls are safe as long as
 *          each uses UDYNLINK_CALL (or an equivalent save/restore).
 */
#define UDYNLINK_CALL(p_func, ret_type, args) \
    ({ \
        uint32_t _udynlink_prev_r9; \
        __asm volatile ("mov %0, r9" : "=r"(_udynlink_prev_r9) : :); \
        UDYNLINK_PREPARE_CALL((p_func)->p_mod); \
        ret_type _udynlink_result = ((ret_type (*)(...))(p_func)->addr) args; \
        __asm volatile ("mov r9, %0" :: "r"(_udynlink_prev_r9) : "r9"); \
        _udynlink_result; \
    })

/**
 * @brief Call a void-returning module function through a reusable handle.
 *
 * Same as UDYNLINK_CALL but for functions that return void.
 *
 * @param[in] p_func   Pointer to a resolved udynlink_func_t handle.
 * @param[in] args     Parenthesized argument list.
 */
#define UDYNLINK_CALL_VOID(p_func, args) \
    ({ \
        uint32_t _udynlink_prev_r9; \
        __asm volatile ("mov %0, r9" : "=r"(_udynlink_prev_r9) : :); \
        UDYNLINK_PREPARE_CALL((p_func)->p_mod); \
        ((void (*)(...))(p_func)->addr) args; \
        __asm volatile ("mov r9, %0" :: "r"(_udynlink_prev_r9) : "r9"); \
    })

/**
 * @brief One-shot macro: lookup, set r9, cast, and call.
 *
 * This is a GCC extension (statement expression).  It resolves the
 * symbol, calls the function (with r9 save/restore), and returns the
 * error code.  The function result is written to @p p_out_ret.
 *
 * @param[in]  p_mod      Pointer to the loaded module.
 * @param[in]  name       Symbol name to look up.
 * @param[in]  ret_type   Return type of the function.
 * @param[in]  args       Parenthesized argument list.
 * @param[out] p_out_ret  Pointer to receive the function result.
 *
 * @return ::UDYNLINK_OK on success, or an error code on failure.
 *         On failure @p p_out_ret receives (ret_type)0.
 *
 * Example:
 * @code
 *   int r;
 *   udynlink_error_t err = UDYNLINK_CALL_MODULE_FUNC(&mod, "add", int, (1, 2), &r);
 * @endcode
 *
 * @note Requires GCC or Clang (uses `({ ... })` statement expression).
 * @note Safe for both prologued and --no-prologue modules because r9
 *       is always saved and restored around the call.
 */
#define UDYNLINK_CALL_MODULE_FUNC(p_mod, name, ret_type, args, p_out_ret) \
    ({ \
        udynlink_func_t _udynlink_hdl = {NULL, 0, NULL}; \
        udynlink_error_t _udynlink_err = udynlink_resolve_func((p_mod), (name), &_udynlink_hdl); \
        if (_udynlink_err == UDYNLINK_OK) { \
            *(p_out_ret) = UDYNLINK_CALL(&_udynlink_hdl, ret_type, args); \
        } else { \
            *(p_out_ret) = (ret_type)0; \
        } \
        _udynlink_err; \
    })

#ifdef __cplusplus
}
#endif

#endif // __UDYNLINK_CALL_H__
