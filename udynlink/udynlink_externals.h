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
#ifdef __cplusplus
extern "C" {
#endif

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
 * Intended contract (not yet wired up): decide whether a given address
 * is writable, which an XIP-mode loader would need before applying
 * data relocations in place.
 *
 * @param p Pointer to inspect.
 *
 * @return Non-zero if @p p points to writable RAM, 0 if it points to
 *         flash/ROM or otherwise non-writable memory.
 *
 * @note A typical implementation compares the address against the
 *       MCU's SRAM region(s).
 *
 * @note A weak default returning 0 (not RAM) is provided.
 *
 * @warning The current loader never calls this hook, and
 *          ::UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED is never returned.
 *          Implementing it today has no effect on load behavior; do
 *          not rely on it for XIP validation.
 */
int udynlink_external_is_pointer_in_ram(const void *p);

/**
 * @brief Allocate memory for a module.
 *
 * Called during udynlink_load_module() once for the module's main RAM
 * block (@p section == NULL) and once per tagged section of a sectioned
 * image (@p section != NULL).  The returned buffer must be at least
 * @p size bytes and aligned to @p align; the loader validates both and
 * fails the load otherwise — ::UDYNLINK_ERR_LOAD_OUT_OF_MEMORY on NULL,
 * ::UDYNLINK_ERR_LOAD_RAM_UNALIGNED for a misaligned main block,
 * ::UDYNLINK_ERR_LOAD_SECTION_UNALIGNED for a misaligned tagged section.
 *
 * @param size    Number of bytes to allocate.
 * @param section Name of the tagged section to place, or NULL for the
 *                module's main RAM block.  The name points into the module
 *                image's symbol string pool and is valid for the duration
 *                of this call only; copy it if the host stores it.
 * @param align   Required alignment in bytes (power of two, >= 4; the
 *                main block uses the maximum alignment over its sections,
 *                which is 4 for modules built without section placement).
 * @param flags   Hint flags from the section table
 *                (::UDYNLINK_SEC_FLAG_NOCACHE et al, plus host-defined
 *                bits 15:8).  0 for the main block; ::UDYNLINK_SEC_FLAG_MAIN
 *                and the reserved bits 23:16 never reach the host.
 *
 * @return Pointer to the allocated memory on success, or NULL on failure
 *         (a NULL for a tagged section fails the load with
 *         ::UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED after everything already
 *         allocated has been freed).
 *
 * @note The memory does not need to be zero-initialized; the loader
 *       zeroes BSS sections itself.
 */
void *udynlink_external_malloc(size_t size, const char *section, size_t align, uint32_t flags);

/**
 * @brief Free memory previously allocated for a module.
 *
 * Called during udynlink_unload_module() and on load error paths for every
 * block obtained from udynlink_external_malloc().  @p section, @p align and
 * @p flags carry exactly the values the allocation was called with, so
 * hosts that route by name, alignment class or flags can demultiplex.
 * If @p p is NULL the call must be a no-op, matching the semantics of the
 * standard free() function.
 *
 * @param p       Pointer to the memory region to free, or NULL.
 * @param section Section name the allocation was made for (see
 *                udynlink_external_malloc()); NULL for a main block.  Only
 *                valid for the duration of the call.
 * @param align   Alignment argument of the matching allocation.
 * @param flags   Flags argument of the matching allocation.
 */
void udynlink_external_free(void *p, const char *section, size_t align, uint32_t flags);

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
 * @note The strings and addresses are read-only and should live in
 *       flash/ROM on embedded targets.
 */
#define UDYNLINK_SYMBOL(sym) { #sym, (void *)(uintptr_t)(sym) }

#ifdef __cplusplus
}
#endif

#endif // #ifndef __UDYNLINK_EXTERNALS_H__
