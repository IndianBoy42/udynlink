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
typedef struct _udynlink_module_t udynlink_module_t;

/**
 * @brief Host integration callbacks.
 *
 * These functions must be implemented by the host firmware (or operating
 * environment) that uses udynlink.  They provide memory management,
 * debug output, and symbol resolution services to the loader.  All
 * callbacks may be invoked from the same context in which the caller
 * invoked udynlink_load_module() or udynlink_unload_module(); on
 * bare-metal Cortex-M targets this is typically thread/interrupt context
 * of the caller, so implementations must be interrupt-safe if modules are
 * loaded from interrupt handlers.
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
 * @brief Resolve a foreign symbol.
 *
 * Called during udynlink_load_module() for each unresolved extern
 * symbol, and during udynlink_lookup_symbol() for weak symbols.
 * This is the sole resolution hook; the host is responsible
 * for all symbol lookup logic (e.g. hash table, linear search, or
 * dynamic resolution).
 *
 * @param p_mod Pointer to the module being loaded or queried.
 *              May be consulted to implement per-module symbol
 *              resolution policies.  The module is partially
 *              initialised at load time (p_header and p_ram are set).
 * @param name  Null-terminated symbol name.
 *
 * @return The absolute address of the symbol if the host provides it,
 *         ::UDYNLINK_SYM_DEFERRED to defer resolution to a later
 *         udynlink_link_incremental() / udynlink_link_symbol() call,
 *         or 0 if the symbol is not found.
 *
 * @note A weak default returning 0 (symbol not found) is provided.
 *       Hosts that load modules without external symbols do not need
 *       to override it.
 */
uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod, const char *name);

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
