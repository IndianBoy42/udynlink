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
 * Why mangled names (`_Z*`) in source instead of source-level
 * `operator new(size_t)` syntax? Two reasons:
 *
 *   1. The header is intended primarily for *C-only* bare-metal hosts that
 *      link no C++ runtime and have no C++ translation unit available.
 *      `operator new` is not expressible in C; the host can only name the
 *      symbol by its Itanium ABI mangled form.
 *   2. The `_Z` prefix is reserved by the C++ standard ([lex.name]) and the
 *      Itanium ABI, so defining these identifiers in source is formally UB.
 *      In practice every C++ runtime (libstdc++, libc++, libsupc++,
 *      picolibc's cxa_* set) defines these exact names — the standard's
 *      "replaceable functions" clause ([support.dynamic]) sanctions user
 *      replacement of `operator new`/`delete` and leaves the symbol name to
 *      the ABI, which mandates exactly `_Znwj`/`_ZdlPv`/etc. on ARM.
 *      GCC (13.x, 15.x) accepts the literal identifiers under `extern "C"`
 *      with `-Wall -Wextra -Wpedantic` and emits them verbatim — confirmed
 *      bit-identical to a source-level `void* operator new(unsigned int)`
 *      definition in a separate probe.
 *
 * `__cxa_pure_virtual` is not a replaceable function; it is an Itanium ABI
 * runtime entry point. The ABI specifies its name and signature; defining it
 * here is exactly the role libsupc++ plays on a hosted target.
 *
 * Override contract: the weak attribute lets a strong definition elsewhere
 * in the link (separate TU; a libstdc++ strong `_Znwj`; a host C++ TU built
 * with source-level `void* operator new(unsigned int)`) win at link time.
 * A strong override MUST live in a separate translation unit from an
 * `#include "udynlink_cpp_abi.h"`: defining both the header's weak stub and
 * a source-level `operator new` in the same TU is rejected by the assembler
 * ("symbol `_Znwj' is already defined"). Verified.
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
 * operator delete / delete[]  (and the aligned + sized variants)
 *
 * GCC emits references to exactly one variant per call site, depending on
 * -fsized-deallocation, the alignment of the type, and the form used
 * (`delete p` vs `delete[] a`). We provide all four so a module that uses
 * any of them loads; the body forwards to udynlink_external_free, ignoring
 * the size and alignment arguments the host's free() does not want.
 * ----------------------------------------------------------------------- */

/* Sized (operator delete(void*, unsigned int)) — emitted when the deleting
 * destructor runs and -fsized-deallocation is in effect. Reachable only if
 * the module actually executes `delete` through a virtual destructor. */
__attribute__((weak))
void _ZdlPvj(void *p, unsigned int sz) {
    (void)sz;
    udynlink_external_free(p);
}

/* Unsized (operator delete(void*)). Emitted when -fsized-deallocation is
 * off (default on bare metal), or when the compiler cannot prove the size. */
__attribute__((weak))
void _ZdlPv(void *p) {
    udynlink_external_free(p);
}

/* Array forms. Behavior mirrors the scalar forms. */
__attribute__((weak))
void _ZdaPvj(void *p, unsigned int sz) {
    (void)sz;
    udynlink_external_free(p);
}

__attribute__((weak))
void _ZdaPv(void *p) {
    udynlink_external_free(p);
}

/* Aligned forms (operator delete(void*, unsigned int, std::align_val_t)).
 * Emitted for types with alignas > __STDCPP_DEFAULT_NEW_ALIGNMENT__ (16).
 * The align_val_t argument is an enum that GCC passes as a plain integer in
 * the third slot; we ignore it and forward to the host free. */
__attribute__((weak))
void _ZdlPvjSt11align_val_t(void *p, unsigned int sz, unsigned int al) {
    (void)sz; (void)al;
    udynlink_external_free(p);
}

__attribute__((weak))
void _ZdaPvjSt11align_val_t(void *p, unsigned int sz, unsigned int al) {
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
void *_Znwj(unsigned int sz) {
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *_Znaj(unsigned int sz) {
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *_ZnwjSt11align_val_t(unsigned int sz, unsigned int al) {
    (void)al;
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *_ZnajSt11align_val_t(unsigned int sz, unsigned int al) {
    (void)al;
    return udynlink_external_malloc(sz);
}

/* nothrow variants (operator new(size_t, std::nothrow_t)). GCC emits these
 * when the source uses `new (std::nothrow) T`. The nothrow_t argument is an
 * empty struct passed by value; in the ARM AAPCS it occupies one register
 * slot, so the signature mirrors the sized form with an extra dummy
 * argument. */
__attribute__((weak))
void *_ZnwjRKSt9nothrow_t(unsigned int sz, void *nt) {
    (void)nt;
    return udynlink_external_malloc(sz);
}

__attribute__((weak))
void *_ZnajRKSt9nothrow_t(unsigned int sz, void *nt) {
    (void)nt;
    return udynlink_external_malloc(sz);
}

/* --------------------------------------------------------------------------
 * __cxa_pure_virtual — the abstract-base vtable slot.
 *
 * Called only if the program commits undefined behavior by dispatching a
 * pure virtual function during base construction or destruction. There is no
 * useful recovery; loop forever so a watchdog can reboot. A host that would
 * rather log+abort can override with a strong definition.
 * ----------------------------------------------------------------------- */
__attribute__((weak, noreturn))
void __cxa_pure_virtual(void) {
    for (;;) { }
}

/* --------------------------------------------------------------------------
 * Resolver: the host calls this from its udynlink_external_resolve_symbol
 * to bind the C++ ABI names above to the addresses of the (weak or
 * host-overridden) implementations. Returns 0 for names this header does
 * not own, so the host's own table can handle the rest of the lookups.
 *
 * Operator new/delete return C++ mangled names, so the strcmp's here use the
 * raw _Z*-prefixed strings the loader passes through verbatim. udynlink
 * does not demangle and the host should not either.
 * ----------------------------------------------------------------------- */
static inline uintptr_t udynlink_cpp_resolve_abi_symbol(const char *name) {
    /* operator delete (sized, unsized, array, aligned). */
    if (!strcmp(name, "_ZdlPvj"))                    return (uintptr_t)&_ZdlPvj;
    if (!strcmp(name, "_ZdlPv"))                     return (uintptr_t)&_ZdlPv;
    if (!strcmp(name, "_ZdaPvj"))                    return (uintptr_t)&_ZdaPvj;
    if (!strcmp(name, "_ZdaPv"))                     return (uintptr_t)&_ZdaPv;
    if (!strcmp(name, "_ZdlPvjSt11align_val_t"))     return (uintptr_t)&_ZdlPvjSt11align_val_t;
    if (!strcmp(name, "_ZdaPvjSt11align_val_t"))     return (uintptr_t)&_ZdaPvjSt11align_val_t;
    /* operator new (scalar, array, aligned, nothrow). */
    if (!strcmp(name, "_Znwj"))                      return (uintptr_t)&_Znwj;
    if (!strcmp(name, "_Znaj"))                      return (uintptr_t)&_Znaj;
    if (!strcmp(name, "_ZnwjSt11align_val_t"))       return (uintptr_t)&_ZnwjSt11align_val_t;
    if (!strcmp(name, "_ZnajSt11align_val_t"))      return (uintptr_t)&_ZnajSt11align_val_t;
    if (!strcmp(name, "_ZnwjRKSt9nothrow_t"))        return (uintptr_t)&_ZnwjRKSt9nothrow_t;
    if (!strcmp(name, "_ZnajRKSt9nothrow_t"))        return (uintptr_t)&_ZnajRKSt9nothrow_t;
    /* pure-virtual slot. */
    if (!strcmp(name, "__cxa_pure_virtual"))         return (uintptr_t)&__cxa_pure_virtual;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* __UDYNLINK_CPP_ABI_H__ */
