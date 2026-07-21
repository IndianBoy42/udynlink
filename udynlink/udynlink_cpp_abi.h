/* udynlink_cpp_abi.h — host-side stubs + resolver for the C++ ABI symbols
 * GCC references from a loadable C++ module compiled with `mkmodule`.
 *
 * A module built with `mkmodule`'s default flags:
 *
 *   -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fno-threadsafe-statics
 *   -fPIE -msingle-pic-base
 *
 * can still pull in the following unresolved `external` symbols even when the
 * runtime paths that use them are never executed:
 *
 *   _ZdlPv   / _ZdlPvj              operator delete(void*) / (void*, unsigned int)
 *   _ZdaPv   / _ZdaPvj              operator delete[](void*) / (void*, unsigned int)
 *   _Znwj    / _Znaj                operator new(unsigned int) / new[](unsigned int)
 *   _ZnwjSt11align_val_t / _ZnajSt11align_val_t   aligned new / new[]
 *   _ZdlPvjSt11align_val_t / _ZdaPvjSt11align_val_t  aligned delete / delete[]
 *   _ZnwjRKSt9nothrow_t / _ZnajRKSt9nothrow_t   nothrow new / new[]
 *   __cxa_pure_virtual                    abstract-base vtable slot
 *
 * The udynlink loader rejects an unresolved `external` symbol with
 * UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL, so the host must bind every one of these
 * that a module references — even when the call site is unreachable at
 * runtime (e.g. the deleting destructor of a `virtual ~T() = default;` class
 * that the module never `delete`s).
 *
 * This header provides reasonable weak defaults for all of them and an inline
 * resolver the host calls from its `udynlink_external_resolve_symbol`:
 *
 *     uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod,
 *                                                 const char *name) {
 *         uintptr_t a = udynlink_cpp_resolve_abi_symbol(name);
 *         if (a) return a;
 *         // ... host's own symbols ...
 *     }
 *
 * Every stub is `__attribute__((weak))`: a host that already links a C++
 * runtime (libstdc++, picolibc's cxa_* collection, or its own
 * implementations) does NOT need to suppress these — the linker keeps the
 * strong version automatically. The weak defaults only fill the gap on a
 * C-only bare-metal host with no C++ runtime at all.
 *
 * Defaults chosen here deliberately do the *safe* thing for a micro:
 *
 *   - new / new[]    forward to `udynlink_external_malloc` (so module heap
 *                    lifetime is observable by the host and consistent with
 *                    the loader's own allocations).
 *   - delete / del[] forward to `udynlink_external_free`. The size and
 *                    align_val arguments are ignored — `udynlink_external_free`
 *                    does not take them. A host that wants sized/aligned free
 *                    can override the relevant symbol with a strong definition.
 *   - __cxa_pure_virtual  loops forever. Calling a pure virtual during
 *                    construction or destruction is undefined behavior; a
 *                    hang is preferable to silent corruption and lets a
 *                    watchdog reboot the system.
 *
 * The `__cxa_guard_acquire` / `_release` / `_abort` trio used to thread-safely
 * initialize function-local statics is NOT provided here. `mkmodule` compiles
 * modules with `-fno-threadsafe-statics`, so GCC emits a plain byte-flag test
 * in the module instead of a call into `__cxa_guard_*` — no host-side stub is
 * needed. This matches udynlink's documented "no thread safety" design
 * (AGENTS.md): initialization-order hazards inside a module are the host's
 * concern via its own synchronization, not the loader's.
 *
 * A note on `__cxa_pure_virtual` and mkmodule's prologue wrapping: if the
 * module *itself* defines `__cxa_pure_virtual` (it should not), `mkmodule`
 * will wrap it as if it were a normal export and you will see a name like
 * `__d7add11ba____cxa_pure_virtual` in the symbol table. `operator delete`
 * and friends are NOT subject to this — `mkmodule` only wraps defined
 * `STB_GLOBAL`/`STB_WEAK` functions, and these are undefined `SHN_UNDEF`
 * references that `mkmodule` classifies as `external`. Do not define
 * `__cxa_pure_virtual` (or any other `__cxa_*`) inside a module; provide it
 * on the host side via this header.
 *
 * Why these stubs use neutral internal names (udynlink_cpp_*) and a resolver,
 * not the raw mangled names (_ZdlPv, _Znwj, ...) the loader actually passes
 * through: udynlink binds external symbols only through the host's
 * `udynlink_external_resolve_symbol` callback — it never links against the
 * host's symbol table by name. So the stub's C identifier is irrelevant to
 * the binding; only what `udynlink_cpp_resolve_abi_symbol(name)` returns
 * matters. Keeping the stub names neutral and non-mangled means:
 *
 *   - A C-only bare-metal host has no C++ runtime and no need to name an
 *     `operator new` in source — it just includes this header.
 *   - A C++ host that links libstdc++ (or defines `operator new` itself)
 *     does *not* silently route module `delete` calls into the host's
 *     `_ZdlPv` via a weak/strong override at link time. The host stays in
 *     explicit control of which `_ZdlPv` the module sees — the udynlink
     resolver returns the address of `udynlink_cpp_delete(void*, unsigned)`
     here, and the host can swap that pointer for its own implementation,
     a sized-free wrapper, a debugging interposer, or the libstdc++ entry
     point — whichever it picks, in code, at the resolution site.
 *
 * The C++ standard [lex.name] reserves `_Z`-prefixed identifiers, and the
 * Itanium ABI further constrains them. Defining the literal mangled names
 * in source works in practice on GCC but is not standard-conforming, so
 * the stubs avoid the question entirely and let the resolver do the
 * mapping. udynlink does not demangle; the resolver compares the raw
 * mangled string the loader passes through.
 */
#ifndef __UDYNLINK_CPP_ABI_H__
#define __UDYNLINK_CPP_ABI_H__

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "udynlink_externals.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Weak defaults for the C++ ABI symbols a `mkmodule`-compiled C++ module
 * references at load time. The stubs have neutral names; the address of
 * each is handed to the loader only via udynlink_cpp_resolve_abi_symbol()
 * below — they are never looked up by name. A strong override is therefore
 * unnecessary and unused; replace behavior by editing the resolver's return
 * value.
 * ----------------------------------------------------------------------- */

/* operator delete(void*, unsigned int) — the sized form GCC emits when
 * -fsized-deallocation is in effect. Reachable only if the module actually
 * executes `delete` through a virtual destructor. Size is dropped because
 * udynlink_external_free takes only the pointer. */
__attribute__((weak))
void udynlink_cpp_delete(void *p, unsigned int sz) {
    (void)sz;
    udynlink_external_free(p);
}

/* operator delete(void*) — the unsized form, emitted when
 * -fsized-deallocation is off (the bare-metal default) or the size is not
 * statically known. */
__attribute__((weak))
void udynlink_cpp_delete_unsized(void *p) {
    udynlink_external_free(p);
}

/* operator delete[](void*, unsigned int) and the unsized form — array
 * counterparts, behavior identical to the scalar forms. */
__attribute__((weak))
void udynlink_cpp_delete_array(void *p, unsigned int sz) {
    (void)sz;
    udynlink_external_free(p);
}

__attribute__((weak))
void udynlink_cpp_delete_array_unsized(void *p) {
    udynlink_external_free(p);
}

/* Aligned forms (operator delete(void*, unsigned int, std::align_val_t)).
 * Emitted for types with alignas > __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16).
 * The align_val_t argument is an enum that GCC passes as a plain integer in
 * the third slot; we ignore it and forward to the host free. */
__attribute__((weak))
void udynlink_cpp_delete_aligned(void *p, unsigned int sz, unsigned int al) {
    (void)sz; (void)al;
    udynlink_external_free(p);
}

__attribute__((weak))
void udynlink_cpp_delete_array_aligned(void *p, unsigned int sz, unsigned int al) {
    (void)sz; (void)al;
    udynlink_external_free(p);
}

/* --------------------------------------------------------------------------
 * operator new / new[]
 *
 * Forward to udynlink_external_malloc. NULL return lets the module's
 * allocation fail the way it would on the host (caller is expected to throw
 * or handle the NULL; with -fno-exceptions the compiler turns `new` into a
 * NULL check + call to a nothrow handler, which we also stub below).
 * ----------------------------------------------------------------------- */
__attribute__((weak))
void *udynlink_cpp_new(unsigned int sz) {
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *udynlink_cpp_new_array(unsigned int sz) {
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *udynlink_cpp_new_aligned(unsigned int sz, unsigned int al) {
    (void)al;
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *udynlink_cpp_new_array_aligned(unsigned int sz, unsigned int al) {
    (void)al;
    return udynlink_external_malloc(sz);
}

/* nothrow variants (operator new(size_t, std::nothrow_t)). GCC emits these
 * when the source uses `new (std::nothrow) T`. The nothrow_t argument is an
 * empty struct passed by value; in the ARM AAPCS it occupies one register
 * slot, so the signature mirrors the sized form with an extra dummy
 * argument. */
__attribute__((weak))
void *udynlink_cpp_new_nothrow(unsigned int sz, void *nt) {
    (void)nt;
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *udynlink_cpp_new_array_nothrow(unsigned int sz, void *nt) {
    (void)nt;
    return udynlink_external_malloc(sz);
}

/* --------------------------------------------------------------------------
 * __cxa_pure_virtual — the abstract-base vtable slot.
 *
 * Called only if the program commits undefined behavior by dispatching a
 * pure virtual function during base construction or destruction. There is no
 * useful recovery; loop forever so a watchdog can reboot.
 * ----------------------------------------------------------------------- */
__attribute__((weak, noreturn))
void udynlink_cpp_pure_virtual(void) {
    for (;;) { }
}

/* --------------------------------------------------------------------------
 * Resolver: the host calls this from its udynlink_external_resolve_symbol
 * to map a module's mangled C++ ABI name to one of the neutral stubs above.
 * Returns 0 for names this header does not own, so the host's own table can
 * handle the rest. Edit the return values here — or copy the function and
 * substitute your own pointers — to override a stub's behavior, route a
 * symbol to libstdc++, interpose for debugging, etc.
 *
 * udynlink does not demangle; the strings compared here are the raw _Z*
 * names the loader passes through verbatim.
 * ----------------------------------------------------------------------- */
static inline uintptr_t udynlink_cpp_resolve_abi_symbol(const char *name) {
    /* operator delete (sized, unsized, array, aligned). */
    if (!strcmp(name, "_ZdlPvj"))                 return (uintptr_t)&udynlink_cpp_delete;
    if (!strcmp(name, "_ZdlPv"))                  return (uintptr_t)&udynlink_cpp_delete_unsized;
    if (!strcmp(name, "_ZdaPvj"))                 return (uintptr_t)&udynlink_cpp_delete_array;
    if (!strcmp(name, "_ZdaPv"))                  return (uintptr_t)&udynlink_cpp_delete_array_unsized;
    if (!strcmp(name, "_ZdlPvjSt11align_val_t"))  return (uintptr_t)&udynlink_cpp_delete_aligned;
    if (!strcmp(name, "_ZdaPvjSt11align_val_t"))  return (uintptr_t)&udynlink_cpp_delete_array_aligned;
    /* operator new (scalar, array, aligned, nothrow). */
    if (!strcmp(name, "_Znwj"))                   return (uintptr_t)&udynlink_cpp_new;
    if (!strcmp(name, "_Znaj"))                  return (uintptr_t)&udynlink_cpp_new_array;
    if (!strcmp(name, "_ZnwjSt11align_val_t"))    return (uintptr_t)&udynlink_cpp_new_aligned;
    if (!strcmp(name, "_ZnajSt11align_val_t"))    return (uintptr_t)&udynlink_cpp_new_array_aligned;
    if (!strcmp(name, "_ZnwjRKSt9nothrow_t"))     return (uintptr_t)&udynlink_cpp_new_nothrow;
    if (!strcmp(name, "_ZnajRKSt9nothrow_t"))     return (uintptr_t)&udynlink_cpp_new_array_nothrow;
    /* pure-virtual slot. */
    if (!strcmp(name, "__cxa_pure_virtual"))      return (uintptr_t)&udynlink_cpp_pure_virtual;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* __UDYNLINK_CPP_ABI_H__ */
