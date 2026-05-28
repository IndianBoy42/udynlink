/* Standalone dependency system for udynlink.
 *
 * Provides cross-module linking via dummy symbols and runtime-generated
 * RAM thunks.  This is an optional layer on top of the v3.0 core; hosts
 * that do not use it pay zero code/RAM cost.
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

#ifndef __UDYNLINK_DEPS_H__
#define __UDYNLINK_DEPS_H__

#include "udynlink.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prefix for dependency declaration symbols.
 * Modules emit EXTERN symbols named ".udynlink.mod.requires.{name}"
 * via the UDYNLINK_REQUIRES() macro.  At load time the core loader
 * treats them like any other unresolved symbol and calls
 * udynlink_external_resolve_symbol().
 */
#define UDYNLINK_DEP_PREFIX             ".udynlink.mod.requires."
#define UDYNLINK_DEP_PREFIX_LEN         24

/* Maximum depth of the circular-dependency detection stack. */
#define UDYNLINK_DEP_MAX_DEPTH          8

/* Size of an inline per-function thunk in bytes. */
#define UDYNLINK_THUNK_SIZE             28

/* ─── Module writer API ─────────────────────────────────────────────── */

/**
 * @brief Declare a dependency on another module.
 *
 * Emits an EXTERN symbol named ".udynlink.mod.requires.{mod_name}".
 * At load time the core loader will call
 * udynlink_external_resolve_symbol() for this symbol. A
 * dependency-aware host returns the module handle address, causing
 * the load to fail if the dependency is not available.
 *
 * @param mod_name Identifier of the required module (not a string).
 */
#define UDYNLINK_REQUIRES(mod_name) \
    extern udynlink_module_t *__udynlink_dep_##mod_name \
    __asm__(".udynlink.mod.requires." #mod_name)

/* ─── Thunk pool ───────────────────────────────────────────────────── */

/**
 * @brief RAM pool for cross-module call thunks.
 *
 * The host provides a contiguous RAM buffer.  Each cross-module
 * function reference gets an inline thunk allocated from this pool
 * (28 bytes each). The pool must be in RAM readable and executable
 * by the MCU.
 */
typedef struct {
    /** Base of the thunk RAM region (host-provided). */
    uint8_t *base;
    /** Total size of the region in bytes. */
    size_t size;
    /** Number of bytes currently allocated. */
    size_t used;
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
 * @brief Allocate @p n bytes from the thunk pool.
 *
 * @param pool Thunk pool to allocate from.
 * @param n    Number of bytes to allocate.
 *
 * @return Pointer to the allocated region, or NULL if the pool is
 *         full.
 */
void *udynlink_thunk_alloc(udynlink_thunk_pool_t *pool, size_t n);

/* ─── Dependency manager ───────────────────────────────────────────── */

/**
 * @brief Dependency manager state.
 *
 * Tracks loaded modules for cross-module symbol resolution.
 * The host allocates and owns this structure.
 */
typedef struct {
    /** Registry of loaded modules (host-owned array). */
    udynlink_module_t **modules;
    /** Number of modules currently registered. */
    size_t count;
    /** Capacity of the @c modules array. */
    size_t capacity;
    /** Circular dependency detection stack (module names). */
    const char *loading_stack[UDYNLINK_DEP_MAX_DEPTH];
    /** Current depth of the loading stack. */
    size_t loading_depth;
} udynlink_dep_mgr_t;

/**
 * @brief Initialise a dependency manager.
 *
 * @param mgr  Pointer to the manager to initialise.
 * @param buf  Host-provided array of module pointers.
 * @param cap  Capacity of @p buf (max modules).
 */
void udynlink_dep_mgr_init(udynlink_dep_mgr_t *mgr,
                           udynlink_module_t **buf, size_t cap);

/* ─── Dependency query helpers ──────────────────────────────────────── */

/**
 * @brief Check whether a symbol name is a dependency declaration.
 *
 * Returns non-zero if @p name starts with ".udynlink.mod.requires.".
 */
int udynlink_dep_is_dependency(const char *name);

/**
 * @brief Extract the module name from a dependency symbol.
 *
 * Skips the ".udynlink.mod.requires." prefix and returns a pointer
 * into @p name at the module name portion.
 */
const char *udynlink_dep_get_name(const char *name);

/**
 * @brief Find a loaded module by name.
 *
 * @param mgr  Dependency manager.
 * @param name Module name to find.
 *
 * @return Pointer to the module handle if found, NULL otherwise.
 */
udynlink_module_t *udynlink_dep_find(udynlink_dep_mgr_t *mgr,
                                     const char *name);

/* ─── Resolution helpers (called from udynlink_external_resolve_symbol) */

/**
 * @brief Resolve a dependency declaration symbol.
 *
 * Called from the host's udynlink_external_resolve_symbol() callback
 * when the symbol name matches the ".udynlink.mod.requires.*" pattern.
 *
 * @param mgr  Dependency manager.
 * @param name Full dependency symbol name.
 *
 * @return The module handle address (cast to uintptr_t) on success,
 *         UDYNLINK_SYM_DEFERRED on circular dependency, or 0 on
 *         failure.
 */
uintptr_t udynlink_dep_resolve_dependency(udynlink_dep_mgr_t *mgr,
                                          const char *name);

/**
 * @brief Resolve a cross-module function symbol via an inline thunk.
 *
 * Searches all loaded dependency modules for @p name.  If found,
 * allocates an inline thunk (28 bytes) from the thunk pool.
 *
 * @param mgr  Dependency manager.
 * @param pool Thunk pool for thunk allocation.
 * @param name Symbol name to resolve.
 *
 * @return Thunk address on success, 0 if the symbol is not found in
 *         any dependency module.
 */
uintptr_t udynlink_dep_resolve_func(udynlink_dep_mgr_t *mgr,
                                    udynlink_thunk_pool_t *pool,
                                    const char *name);

/**
 * @brief Resolve a cross-module data symbol.
 *
 * Searches all loaded dependency modules for @p name.  Data symbols
 * do not require thunks — the absolute address is returned directly.
 */
uintptr_t udynlink_dep_resolve_data(udynlink_dep_mgr_t *mgr,
                                    const char *name);

/**
 * @brief Register a module in the dependency manager after loading.
 *
 * Useful if the host loads modules via udynlink_load_module() instead
 * of udynlink_dep_load().
 */
void udynlink_dep_register(udynlink_dep_mgr_t *mgr,
                           udynlink_module_t *p_mod);

/* ─── Module load / unload with dependency tracking ─────────────────── */

/**
 * @brief Load a module with automatic dependency detection.
 *
 * Pushes the module onto the loading stack (for circular dependency
 * detection), calls udynlink_load_module(), registers the module in
 * the dependency manager, then pops the loading stack.
 */
udynlink_error_t udynlink_dep_load(udynlink_dep_mgr_t *mgr,
                                   udynlink_module_t *p_mod,
                                   const void *base_addr,
                                   void *load_addr, size_t load_size,
                                   udynlink_load_mode_t load_mode,
                                   udynlink_thunk_pool_t *pool);

/**
 * @brief Unload a module and deregister it from the dependency manager.
 *
 * Removes the module from the registry, then calls
 * udynlink_unload_module().
 */
udynlink_error_t udynlink_dep_unload(udynlink_dep_mgr_t *mgr,
                                     udynlink_module_t *p_mod);

#ifdef __cplusplus
}
#endif

#endif /* __UDYNLINK_DEPS_H__ */
