/* Module-facing API for the udynlink dependency system.
 *
 * This is the only udynlink header a *module source* needs: it provides
 * UDYNLINK_REQUIRES() for cross-module dependency declarations and
 * UDYNLINK_THUNK_GATEWAY()/UDYNLINK_THUNK_EXPORT() for preallocated
 * thunk exports.  It is deliberately self-contained (standard headers
 * only) so module builds need just one -I entry pointing at the udynlink
 * include dir — module sources must not be forced to include the
 * host-facing udynlink_deps.h / udynlink.h.
 *
 * Hosts implement the runtime side of these macros in udynlink_deps.h /
 * udynlink_deps.c; this header contains no host API.
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

#ifndef __UDYNLINK_DEPS_API_H__
#define __UDYNLINK_DEPS_API_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Module writer API ─────────────────────────────────────────────── */

/**
 * @brief Declare a dependency on another module.
 *
 * Emits a GOT-referenced extern symbol named
 * ".udynlink.mod.requires.{mod_name}" plus a dummy function that
 * forces the compiler to emit an R_ARM_GOT_BREL (LOT) relocation for
 * the symbol. This ensures:
 *
 * - The core loader's external symbol resolver processes it via
 *   udynlink_external_resolve_symbol()
 * - mkmodule places the relocation before function EXTERN relocations
 *   so the dependency is resolved first, enabling auto-loading
 * - --gc-sections keeps the symbol alive via the linked section
 *
 * @param mod_name Identifier of the required module (not a string).
 *
 * @note This macro does not require any header includes beyond
 *       stdint.h/stddef.h. It uses only compiler built-in attributes
 *       and asm labels.
 */
#define UDYNLINK_REQUIRES(mod_name) \
    typedef void (*_udynlink_dep_fn_##mod_name)(void); \
    _udynlink_dep_fn_##mod_name _udynlink_dep_##mod_name \
        __asm__(".udynlink.mod.requires." #mod_name); \
    __attribute__((used)) void _udynlink_dep_ref_##mod_name(void) { \
        volatile _udynlink_dep_fn_##mod_name f = _udynlink_dep_##mod_name; \
        (void)f; \
    }

/* ─── Preallocated thunk exports ───────────────────────────────────── */

/* Prefix for thunk-export marker symbols.
 * UDYNLINK_THUNK_EXPORT(fn) reserves a stub slot named
 * ".udynlink.thunk_export.{fn}" in the module's .bss.  At load time
 * udynlink_dep_generate_thunks() fills each slot with a stub that
 * branches to the module's gateway slot ("udynlink_thunk_gateway").
 */
#define UDYNLINK_THUNK_EXPORT_PREFIX      ".udynlink.thunk_export."
#define UDYNLINK_THUNK_EXPORT_PREFIX_LEN  23

/* Max symbol-name length the resolver builds a prefixed marker name into. */
#define UDYNLINK_DEP_MAX_NAME             64

/* Reserve one 18-byte gateway slot in the module's .bss.  Exactly one per
 * module.  udynlink_dep_generate_thunks() fills it with the gateway bytes
 * (ram_base patched in) at load time.
 *
 * @note The slot sizes are literals (18/10) so this macro stays
 *       self-contained: module sources can paste it without including any
 *       udynlink header, mirroring UDYNLINK_REQUIRES().  The _Static_assert
 *       in udynlink_deps.h keeps the literals in sync with
 *       udynlink_thunk.h.
 *
 * @note The slots are unreferenced data, so --gc-sections would drop them.
 *       The toolchain keeps them via the KEEP(*(.bss.udynlink_thunk_pool))
 *       directive in scripts/code_before_data.ld (GCC's __attribute__
 *       ((retain)) is not honored for variables by arm-none-eabi-gcc).
 */
#define UDYNLINK_THUNK_GATEWAY() \
    __attribute__((used, section(".bss.udynlink_thunk_pool"), aligned(2))) \
    uint8_t _udynlink_thunk_gateway[18] \
        __asm__("udynlink_thunk_gateway")

/* Reserve one 10-byte stub slot per exported function <fn>.  The host fills
 * it at load with a stub that loads <fn>'s address into ip and branches to
 * the module's gateway.  <fn> must be an exported function of this module.
 *
 * @note Kept alive by KEEP(*(.bss.udynlink_thunk_pool)) in
 *       scripts/code_before_data.ld; see UDYNLINK_THUNK_GATEWAY().
 */
#define UDYNLINK_THUNK_EXPORT(fn) \
    __attribute__((used, section(".bss.udynlink_thunk_pool"), aligned(2))) \
    uint8_t _udynlink_thunk_slot_##fn[10] \
        __asm__(UDYNLINK_THUNK_EXPORT_PREFIX #fn)

#ifdef __cplusplus
}
#endif

#endif /* __UDYNLINK_DEPS_API_H__ */
