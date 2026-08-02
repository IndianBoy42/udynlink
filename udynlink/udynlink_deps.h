/* Standalone dependency system for udynlink.
 *
 * Provides cross-module linking via dummy symbols and runtime-generated
 * RAM thunks.  This is an optional layer on top of the v3.0 core; hosts
 * that do not use it pay zero code/RAM cost.
 *
 * Thunk design: two-level dispatch with per-module gateways (18 bytes)
 * and per-function stubs (10 bytes).  The stub loads the target
 * function address into R12 (IP) via movw+movt, then branches to the
 * module's shared gateway which switches R9 and calls the function.
 * This preserves R0-R3 (argument registers) and saves ~14 bytes per
 * cross-module function reference compared to inline thunks.
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

/* Module-facing macros (UDYNLINK_REQUIRES, UDYNLINK_THUNK_GATEWAY,
 * UDYNLINK_THUNK_EXPORT) live in udynlink_deps_api.h so module sources
 * can include that single self-contained header instead of this one. */
#include "udynlink_deps_api.h"
#include "udynlink_thunk.h"
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
#define UDYNLINK_DEP_PREFIX_LEN         23

/* Maximum depth of the circular-dependency detection stack. */
#define UDYNLINK_DEP_MAX_DEPTH          8

/* Guard the thunk-export slot literals (18/10) in udynlink_deps_api.h
 * against udynlink_thunk.h size changes. */
#ifdef __cplusplus
static_assert(UDYNLINK_GATEWAY_SIZE == 18 && UDYNLINK_STUB_SIZE == 10,
              "thunk-export macro sizes must track udynlink_thunk.h");
#else
_Static_assert(UDYNLINK_GATEWAY_SIZE == 18 && UDYNLINK_STUB_SIZE == 10,
               "thunk-export macro sizes must track udynlink_thunk.h");
#endif

/* ─── Dependency manager ───────────────────────────────────────────── */

/**
 * @brief Per-module entry in the dependency manager.
 *
 * Tracks the module handle and its allocated gateway (if any).
 */
typedef struct {
    /** Loaded module handle. */
    udynlink_module_t *p_mod;
    /** Allocated gateway in the thunk pool, or NULL. */
    uint8_t *gateway;
} udynlink_dep_entry_t;

/**
 * @brief Host-provided callback to load a missing dependency module.
 *
 * Called by udynlink_dep_resolve_dependency() when a required module
 * is not yet loaded.  The host should locate the module image,
 * allocate a udynlink_module_t, and call udynlink_dep_load() (or
 * udynlink_load_module() + udynlink_dep_register()).
 *
 * A weak no-op default returning NULL is provided.  Hosts that want
 * automatic dependency loading must override this function.
 *
 * @param name Module name (without the .udynlink.mod.requires. prefix).
 *
 * @return Pointer to the loaded module handle, or NULL if the
 *         dependency could not be loaded.
 */
udynlink_module_t *udynlink_external_dep_load(const char *name);

/**
 * @brief Dependency manager state.
 *
 * Tracks loaded modules for cross-module symbol resolution.
 * The host allocates and owns this structure.
 */
typedef struct {
    /** Registry of loaded modules (host-owned array). */
    udynlink_dep_entry_t *entries;
    /** Number of modules currently registered. */
    size_t count;
    /** Capacity of the @c entries array. */
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
 * @param buf  Host-provided array of module entries.
 * @param cap  Capacity of @p buf (max modules).
 */
void udynlink_dep_mgr_init(udynlink_dep_mgr_t *mgr,
                           udynlink_dep_entry_t *buf, size_t cap);

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
 *         failure (dependency not found or udynlink_external_dep_load
 *         returned NULL).
 */
uintptr_t udynlink_dep_resolve_dependency(udynlink_dep_mgr_t *mgr,
                                           const char *name);

/**
 * @brief Resolve a cross-module function symbol via a thunk stub.
 *
 * Searches all loaded dependency modules for @p name.  If found,
 * allocates a gateway (18 bytes, one per module) and a stub
 * (10 bytes, one per function) from the thunk pool.
 *
 * The stub loads the function address into R12 (IP) via movw+movt
 * and branches to the module's shared gateway, which switches R9
 * and calls the function.  This preserves R0-R3 (argument registers).
 *
 * @param mgr  Dependency manager.
 * @param pool Thunk pool for thunk allocation.
 * @param name Symbol name to resolve.
 *
 * @return Stub address (with Thumb bit set) on success, 0 if the
 *         symbol is not found in any dependency module.
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

/**
 * @brief Eagerly generate the in-module thunks of a module's declared
 *        thunk exports.
 *
 * Modules declare thunk exports with UDYNLINK_THUNK_GATEWAY() +
 * UDYNLINK_THUNK_EXPORT(fn), which reserve stub slots in the module's
 * own .bss.  This function writes the gateway (18 bytes, ram_base
 * patched in) and one stub (10 bytes) per declared export into those
 * slots, so cross-module importers can be served without touching the
 * dynamic thunk pool.
 *
 * Called automatically by udynlink_dep_load() right after the module is
 * registered; a no-op for modules without a "udynlink_thunk_gateway"
 * symbol.  It is idempotent and public so the host can re-run it after
 * udynlink_relocate_module(): the move preserves the PC-relative
 * stub->gateway branches, but the absolute immediates (the gateway's
 * ram_base literal and each stub's function address in COPY_ALL /
 * COPY_TEXT_DATA) go stale and must be re-patched.
 *
 * @param mgr   Dependency manager the module is registered in.
 * @param p_mod Loaded module handle.
 */
void udynlink_dep_generate_thunks(udynlink_dep_mgr_t *mgr,
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
