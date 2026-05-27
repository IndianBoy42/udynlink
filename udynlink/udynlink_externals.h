/* ARM Cortex-M micro dynamic linker (udynlink) external function prototypes.
 *
 * Copyright (c) 2016 Bogdan Marinescu
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

#ifndef __UDYNLINK_EXTERNALS_H__
#define __UDYNLINK_EXTERNALS_H__

#include <stddef.h>
#include <stdarg.h>
#include "stdint.h"

struct _udynlink_module_t;

/**
 * @brief Host integration callbacks.
 *
 * These functions must be implemented by the host firmware (or operating
 * environment) that uses udynlink.  They provide memory management,
 * debug output, symbol resolution, and dependency lookup services to
 * the loader.  All callbacks may be invoked from the same context in
 * which the caller invoked udynlink_load_module() or
 * udynlink_unload_module(); on bare-metal Cortex-M targets this is
 * typically thread/interrupt context of the caller, so implementations
 * must be interrupt-safe if modules are loaded from interrupt handlers.
 */

/**
 * @brief Determine whether a pointer references RAM.
 *
 * Called during udynlink_load_module() to decide whether a given
 * address is writable.  This is critical for XIP mode: the loader must
 * know if it can safely apply data relocations in place.
 *
 * @param p Pointer to inspect.
 *
 * @return Non-zero if @p p points to writable RAM, 0 if it points to
 *         flash/ROM or otherwise non-writable memory.
 *
 * @note A typical implementation compares the address against the
 *       MCU's SRAM region(s).
 *
 * @note A weak default returning 0 (not RAM) is provided.  Hosts that
 *       use XIP mode must override it with a real implementation.
 */
int udynlink_external_is_pointer_in_ram(const void *p);

/**
 * @brief Allocate memory for a module's RAM region.
 *
 * Called during udynlink_load_module() to obtain the backing memory
 * for the module's LOT, .data, .bss, and optionally .text (depending
 * on the load mode).  The returned buffer must be at least @p size
 * bytes.
 *
 * @param size Number of bytes to allocate.
 *
 * @return Pointer to the allocated memory on success, or NULL on
 *         failure.
 *
 * @note The memory does not need to be zero-initialized; the loader
 *       zeroes BSS separately.
 */
void *udynlink_external_malloc(size_t size);

/**
 * @brief Free memory previously allocated for a module.
 *
 * Called during udynlink_unload_module() to release memory that was
 * obtained from udynlink_external_malloc().  If @p p is NULL the call
 * must be a no-op, matching the semantics of the standard free()
 * function.
 *
 * @param p Pointer to the memory region to free, or NULL.
 */
void udynlink_external_free(void *p);

/**
 * @brief Output a debug message from the loader.
 *
 * Called by the internal debug subsystem whenever
 * udynlink_set_debug_level() is set above ::UDYNLINK_DEBUG_NONE.
 * The implementation should format and emit the message.
 *
 * @param s  Printf-style format string.
 * @param va Varargs list with the format arguments.
 *
 * @note A weak no-op default is provided.  Hosts only need to
 *       implement this if debug output is desired.
 */
void udynlink_external_vprintf(const char *s, va_list va);

/**
 * @brief Resolve a foreign symbol (primary / fallback tier).
 *
 * Called during udynlink_load_module() for each unresolved extern
 * symbol after the critical-symbol callback and after searching
 * dependency modules.  This is the final tier of the three-tier
 * resolution chain.
 *
 * @param name Null-terminated symbol name.
 *
 * @return The absolute address of the symbol if the host provides it,
 *         or 0 if the symbol is not found.
 *
 * @note A weak default returning 0 (symbol not found) is provided.
 *       Hosts that load modules without external symbols do not need
 *       to override it.
 */
uint32_t udynlink_external_resolve_symbol(const char *name);

/**
 * @brief Resolve a foreign symbol (critical / host-only tier).
 *
 * Called during udynlink_load_module() for each unresolved extern
 * symbol BEFORE searching dependency modules and BEFORE
 * udynlink_external_resolve_symbol().  Use this for symbols that must
 * always be supplied by the host firmware (e.g., core system services)
 * and must never be satisfied by another loaded module.
 *
 * @param name Null-terminated symbol name.
 *
 * @return The absolute address of the symbol if the host provides it,
 *         or 0 to let the resolution chain continue.
 *
 * @note A weak default returning 0 is provided.  Hosts that do not
 *       need the critical-symbol tier do not need to override it.
 */
uint32_t udynlink_external_resolve_critical_symbol(const char *name);

/**
 * @brief Look up a loaded module by name.
 *
 * Called during udynlink_load_module() when a module declares
 * dependencies (via `mkmodule --depends`).  The loader checks that
 * every named dependency is already loaded before it proceeds.
 *
 * @param module_name Null-terminated module name.
 *
 * @return Pointer to the module handle (::udynlink_module_t) if the
 *         dependency is loaded, or NULL if it is not found.
 *
 * @note If any required dependency is not found,
 *       udynlink_load_module() fails with
 *       ::UDYNLINK_ERR_LOAD_MISSING_DEP.
 *
 * @note A weak default returning NULL is provided.  Hosts that do not
 *       use module dependencies do not need to override it.
 */
struct _udynlink_module_t *udynlink_external_get_module_handle(const char *module_name);

/**
 * @brief Check if a module is currently being loaded.
 *
 * Called during dependency validation. If a module declares a dependency
 * on a module that is already in the middle of being loaded, a circular
 * dependency exists.
 *
 * @param module_name Null-terminated module name.
 * @return Non-zero if a load for this module name is in progress, 0 otherwise.
 *
 * @note A weak default returning 0 is provided. Hosts that track load
 *       state can override this to enable cycle detection.
 */
int udynlink_external_is_module_loading(const char *module_name);

/**
/**
 * @brief Convenience macro for building host symbol tables.
 *
 * Expands to an initializer for a struct containing a symbol name
 * string and its address.  Typical usage:
 * @code
 *   const my_symbol_entry_t my_syms[] = {
 *       UDYNLINK_SYMBOL(printf),
 *       UDYNLINK_SYMBOL(malloc),
 *   };
 * @endcode
 *
 * @param sym The bare symbol name (not a string).
 */
#define UDYNLINK_SYMBOL(sym) { #sym, (void *)(uintptr_t)(sym) }

#endif // #ifndef __UDYNLINK_EXTERNALS_H__
