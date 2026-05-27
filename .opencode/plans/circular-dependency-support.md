# Plan: Backwards-Compatible Circular Dependency Support

> **Status**: Phase 0 (detection & rejection) is **COMPLETE**. This plan describes the opt-in extension for hosts that need to load circular dependency graphs.
>
> **API Note**: The plan below references `udynlink_link_dependency()`. The final implementation replaced this with two lower-level primitives — `udynlink_link_incremental()` and `udynlink_relink_all()` — plus the host manually populating `p_mod->deps[]`. See the user-facing docs for the current API.

## Current State (As of Today)

The loader already detects and **rejects** circular dependencies by default:

| Feature | Status | Details |
|---------|--------|---------|
| `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` | ✅ Done | Error code added to enum |
| Self-dependency detection | ✅ Always active | `strcmp` against current module name in `udynlink_load_module()` |
| Cross-module cycle detection | ✅ Opt-in via host callback | `udynlink_external_is_module_loading()` weak callback; host overrides to enable |
| `udynlink_get_module_deps()` | ✅ Done | Read-only header inspection helper (no loading required) |
| Documentation | ✅ Done | Failure mode documented; circular deps discouraged |
| Integration test | ✅ Done | `test-circular-deps` verifies self-dep rejection |

**Default behavior**: `udynlink_load_module()` fails with `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` when a cycle is detected. This is the **recommended** behavior for most systems.

## Goal

Provide an **opt-in, backwards-compatible** path for hosts that need to load circular dependency graphs or handle **optional dependencies**. The default `udynlink_load_module()` remains strict.

## Design Principles

1. **Never break existing behavior.** `udynlink_load_module()` keeps rejecting cycles.
2. **Opt-in via the existing callback contract.** The host signals "defer this dep" through `udynlink_external_get_module_handle()`, not through new load functions.
3. **Loader stays stateless.** The host orchestrates load order and tracks state.
4. **Refcount semantics are unchanged.** Circular deps still create un-unloadable modules. Document this clearly.
5. **Minimal API surface.** Only add what's strictly necessary.
6. **Optional deps are a natural consequence.** If a dependency is deferred and never linked, the module falls back to host symbols (or NULL). No extra mechanism needed.

---

## New Design: Sentinel + `link_dependency()`

### Core Insight

Instead of splitting `udynlink_load_module()` into two new functions (`load_module_deferred` + `link_module`), we keep the existing load function and let the **host control per-dependency linking** via the existing callback.

### 1. `UDYNLINK_DEP_DEFERRED` Sentinel

```c
#define UDYNLINK_DEP_DEFERRED ((udynlink_module_t*)1)
```

**Rationale**: `(udynlink_module_t*)1` is safe on ARM Cortex-M because:
- It is not 4-byte aligned, so it can never be a valid heap-allocated struct pointer
- It is in the vector table region, never where the host places module handles
- Existing hosts never return it, so behavior is unchanged for all existing code

**Contract change**: `udynlink_external_get_module_handle()` may now return three values:
- Valid pointer → dependency is loaded and will be linked immediately (existing behavior)
- `NULL` → dependency not found, load fails with `UDYNLINK_ERR_LOAD_MISSING_DEP` (existing behavior)
- `UDYNLINK_DEP_DEFERRED` → dependency exists but should not be linked yet. Loader skips it.

#### 1a. `UDYNLINK_SYM_DEFERRED` Sentinel for Symbol Resolution

```c
#define UDYNLINK_SYM_DEFERRED ((uint32_t)1)
```

**Rationale**: The same pattern applies to the **three-tier symbol resolution chain**. The host may want to defer resolution of an individual `EXTERN` symbol rather than fail the entire module load.

**Contract change**: `udynlink_external_resolve_symbol()` and `udynlink_external_resolve_critical_symbol()` may now return `UDYNLINK_SYM_DEFERRED`:
- Valid address → symbol resolved, written to relocation slot (existing behavior)
- `0` → symbol not found, load fails with `UDYNLINK_ERR_LOAD_CANT_RESOLVE` (existing behavior)
- `UDYNLINK_SYM_DEFERRED` → symbol is known but not yet available. Loader writes `0` to the relocation slot and **continues loading** (does not fail).

**When this is useful**:
- A module references a host function that will be registered later (e.g., after hardware initialization)
- A module references a symbol from another module that hasn't been loaded yet, and the host wants to allow this (even without using `UDYNLINK_DEP_DEFERRED` for the whole dependency)
- Optional symbols that the module can gracefully handle being NULL

**Interaction with `link_dependency()`**: When `link_dependency()` re-runs the three-tier chain for EXTERN relocations, deferred symbols are attempted again. If the host now returns a real address, the slot is updated. If it still returns `UDYNLINK_SYM_DEFERRED`, the slot stays at `0`.

**Important**: The critical-symbol callback (`udynlink_external_resolve_critical_symbol`) returning `UDYNLINK_SYM_DEFERRED` is treated the same way — the load does not fail, and the symbol is deferred. This is a change from the current behavior where critical symbols are, well, critical. **Hosts should only return this sentinel from the critical callback if they truly intend to defer a critical symbol.**

### 2. Modified `udynlink_load_module()` Dependency Loop

Inside `udynlink_load_module()` and `udynlink_load_module_from_stream()`:

```c
for (int d = 0; d < p_header->num_deps; d++) {
    const char *dep_name = ...;
    udynlink_module_t *dep_mod = udynlink_external_get_module_handle(dep_name);
    if (dep_mod == NULL) {
        err = UDYNLINK_ERR_LOAD_MISSING_DEP;
        goto cleanup;
    }
    if (dep_mod == UDYNLINK_DEP_DEFERRED) {
        // Skip: don't add to deps[], don't increment num_deps, don't increment refcount
        continue;
    }
    // Normal linking
    p_mod->deps[p_mod->num_deps++] = dep_mod;
    dep_mod->dep_refcount++;
}
```

**Why this works**: The relocation loop for `EXTERN` symbols (tier 2: search dependency modules) scans `p_mod->deps[]`. Since deferred deps aren't in `deps[]`, those symbols naturally fall through to tier 3 (host fallback). No changes needed to the relocation loop.

### 3. New `udynlink_link_dependency()`

```c
udynlink_error_t udynlink_link_dependency(udynlink_module_t *a, udynlink_module_t *b);
```

**Behavior** (symmetric — checks both directions):

1. Check if `a`'s header lists `b` as a dependency and `b` is not yet in `a->deps[]`:
   - If so: add `b` to `a->deps[]`, increment `b->dep_refcount`, then **re-resolve all `EXTERN` relocations in `a`**
2. Check if `b`'s header lists `a` as a dependency and `a` is not yet in `b->deps[]`:
   - If so: add `a` to `b->deps[]`, increment `a->dep_refcount`, then **re-resolve all `EXTERN` relocations in `b`**
3. Return `UDYNLINK_OK` even if neither direction applies (idempotent on unrelated modules or already-linked pairs).

**Why re-resolve all EXTERNs**: During initial load, an `EXTERN` symbol might have resolved from the host fallback (tier 3) because the dep wasn't in `deps[]` yet. After linking the dep, that dep (tier 2) should take precedence per the three-tier spec. A full re-scan is correct, safe, and the overhead is negligible for typical embedded modules.

### 4. New `udynlink_link_symbol()`

```c
udynlink_error_t udynlink_link_symbol(
    udynlink_module_t *p_mod,
    const char *sym_name,
    uint32_t sym_addr
);
```

**Behavior**:
- Scans the module's relocation table for entries referencing `sym_name`.
- For each matching relocation (any type — LOT, data, function pointer), writes `sym_addr` directly to the relocation slot.
- Works regardless of whether the symbol was previously resolved or not. Overwrites existing values.
- Does **not** touch `p_mod->deps[]`, `num_deps`, or `dep_refcount`. This is a low-level patch, not a dependency link.
- Returns `UDYNLINK_ERR_LOAD_CANT_RESOLVE` if the symbol is not found in the relocation table.

**Use cases**:
1. **Deferred host symbols**: The host knows a symbol's address now and wants to patch it directly, without re-running the full three-tier resolution chain.
2. **Hot-patching**: Replace a module's extern reference with a different implementation at runtime (e.g., mock for testing).
3. **Dynamic symbol tables**: Host maintains its own symbol table and pushes updates into loaded modules.

**Example**:

```c
// Host has deferred late_init during load
// Now the address is known
udynlink_link_symbol(&mod, "late_init", (uint32_t)(uintptr_t)&real_late_init);
```

**Interaction with `link_dependency()`**: `link_symbol()` is orthogonal. A host can call `link_symbol()` on its own, or after `link_dependency()` to override a specific resolution. Since `link_symbol()` patches the relocation slot directly, it bypasses the three-tier chain entirely.

### 5. Optional Dependency Detection Helpers

To let hosts and modules query whether a dependency or symbol has been linked, add three small query APIs:

#### `udynlink_is_module_fully_linked()`

```c
/**
 * @brief Check whether all declared dependencies of a module are linked.
 *
 * @param[in] p_mod Pointer to a loaded module.
 *
 * @return 1 if every dependency declared in the module header is present
 *         in p_mod->deps[], 0 otherwise.
 */
int udynlink_is_module_fully_linked(const udynlink_module_t *p_mod);
```

#### `udynlink_get_linked_dependency()`

```c
/**
 * @brief Return the handle of a linked dependency by name.
 *
 * @param[in] p_mod    Pointer to a loaded module.
 * @param[in] dep_name Null-terminated dependency name.
 *
 * @return Pointer to the dependency module if it is linked into @p p_mod,
 *         or NULL if the dependency is not linked (either deferred or
 *         not declared).
 */
udynlink_module_t *udynlink_get_linked_dependency(const udynlink_module_t *p_mod, const char *dep_name);
```

#### `udynlink_is_symbol_resolved()`

```c
/**
 * @brief Check whether an extern symbol has a non-zero resolved value.
 *
 * A symbol that was deferred during load and has not yet been linked
 * will resolve to 0 (or the host fallback value). After
 * udynlink_link_dependency() resolves it from a dependency module,
 * this function returns 1.
 *
 * @param[in] p_mod    Pointer to a loaded module.
 * @param[in] sym_name Null-terminated symbol name.
 *
 * @return 1 if the symbol exists and its value is non-zero, 0 otherwise.
 */
int udynlink_is_symbol_resolved(const udynlink_module_t *p_mod, const char *sym_name);
```

**How modules detect optional dependencies**: In udynlink's PIE model, every `extern` function access goes through the LOT — the compiler generates a load from the LOT slot to get the function's address, then calls it. When an optional dependency is deferred and not yet linked, the LOT slot contains `0` (our placeholder). This means the module CAN check at runtime by treating the extern symbol as a function pointer:

```c
// Inside a module that optionally depends on "logging"
extern void log_printf(const char *fmt, ...);

void my_init(void) {
    // In C, 'log_printf' in an expression context decays to a pointer.
    // The compiler emits code to load that pointer from the LOT slot.
    // If the dep is deferred, the LOT slot is 0.
    if (log_printf != NULL) {
        log_printf("module initialized\n");
    }
}
```

**Caveats**:
- This works only if the host does **not** provide a fallback stub for `log_printf` via `udynlink_external_resolve_symbol()`. If the host returns a stub address, the LOT slot is non-zero and the module cannot distinguish "stub" from "real dependency linked" using this pattern alone.
- If the host provides a weak default stub that returns 0, modules should use a dedicated sentinel or query API instead.

**Alternative for host-mediated detection**: The host can expose `udynlink_is_symbol_resolved()` as a host symbol so modules can query explicitly:

```c
extern int udynlink_is_symbol_resolved(const void *mod, const char *name);
extern void log_printf(const char *fmt, ...);

void my_init(void) {
    if (udynlink_is_symbol_resolved(NULL, "log_printf")) {
        log_printf("module initialized\n");
    }
}
```

**Host-side queries** (before calling module functions):

```c
if (!udynlink_is_module_fully_linked(&my_mod)) {
    printf("warning: my_mod has unlinked dependencies\n");
}

udynlink_module_t *logging = udynlink_get_linked_dependency(&my_mod, "logging");
if (logging == NULL) {
    printf("logging not yet linked\n");
}
```

---

## Concrete Example: Circular Pair A ↔ B

```c
// Host callback
udynlink_module_t *udynlink_external_get_module_handle(const char *name) {
    // Check if already loaded and fully linked
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i] && strcmp(udynlink_get_module_name(g_modules[i]), name) == 0)
            return g_modules[i];
    }
    // Is it in the "loading" set? (deferred)
    for (int i = 0; i < g_loading_count; i++) {
        if (strcmp(g_loading_names[i], name) == 0)
            return UDYNLINK_DEP_DEFERRED;
    }
    return NULL;
}

// 1. Load A (B is not loaded yet, so callback returns DEFERRED for B)
udynlink_load_module(&mod_a, mod_a_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&mod_a);

// 2. Load B (A is already loaded, so callback returns &mod_a)
// A is linked into B immediately because it's already loaded
udynlink_load_module(&mod_b, mod_b_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&mod_b);
// At this point: B depends on A (linked). A depends on B (deferred).

// 3. Link the deferred direction
udynlink_link_dependency(&mod_a, &mod_b);
// At this point: both directions are linked.
// mod_a.dep_refcount == 1, mod_b.dep_refcount == 1
```

**Alternative host strategy**: Both can be deferred:

```c
// Pre-register both as "loading"
g_loading_names[0] = "mod_a";
g_loading_names[1] = "mod_b";

// Both loads see DEFERRED for the other
udynlink_load_module(&mod_a, mod_a_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&mod_a);
udynlink_load_module(&mod_b, mod_b_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&mod_b);

// Now link both directions at once
udynlink_link_dependency(&mod_a, &mod_b);
```

## Concrete Example: Optional Dependency

```c
// Host callback
udynlink_module_t *udynlink_external_get_module_handle(const char *name) {
    if (strcmp(name, "logging") == 0) {
        if (logging_loaded) return &logging_mod;
        // Optional: don't fail load, just defer
        return UDYNLINK_DEP_DEFERRED;
    }
    // ... other deps
}

// Load my_module (logging is deferred)
udynlink_load_module(&my_mod, my_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);

// my_module's extern log_printf resolves to 0 (host fallback doesn't provide it)
// my_module checks: if (log_printf) log_printf("hello"); // safely no-op

// Later, host decides to load logging
udynlink_load_module(&logging_mod, logging_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&logging_mod);

// Link the optional dependency
udynlink_link_dependency(&my_mod, &logging_mod);

// Now my_module's log_printf resolves to logging_mod's exported symbol
// my_module calls it and it works
```

---

## Backwards Compatibility Analysis

| Aspect | Before | After Phase 1 | After Phase 2 |
|--------|--------|---------------|---------------|
| `udynlink_load_module()` | Rejects cycles | **Unchanged** — still rejects cycles unless host returns `UDYNLINK_DEP_DEFERRED` | **Unchanged** |
| `udynlink_load_module_from_stream()` | Rejects cycles | **Unchanged** — same callback contract | **Unchanged** |
| Error codes | `MISSING_DEP` for missing deps | **Unchanged** — `CIRCULAR_DEP` still emitted for detected cycles | **Unchanged** |
| Host callbacks | 7 required (6 + 1 optional weak) | **Unchanged** — `is_module_loading` still optional; `get_module_handle` gains new valid return value | **Unchanged** |
| `dep_refcount` semantics | Incremented on load | **Unchanged** — only incremented when dep is actually linked (normal or via `link_dependency`) | **Unchanged** |
| Existing tests | All pass | All pass + new `link_dependency` tests | All pass + new optional-dep tests |

**Guarantee**: No existing host firmware needs to change. The new sentinel is only returned by hosts that explicitly choose to use it.

---

## Migration Path for Hosts

### Stage 0: Today (No Changes Needed)

Use `udynlink_load_module()` as always. Circular deps are rejected. Optional deps are impossible (load fails if dependency is missing).

### Stage 1: Opt-In Deferred Loading

For a circular pair `mod_a ↔ mod_b`:

```c
// Host pre-registers both as "loading" so callback returns DEFERRED
g_loading_names[0] = "mod_a";
g_loading_names[1] = "mod_b";

udynlink_load_module(&mod_a, mod_a_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&mod_a);

udynlink_load_module(&mod_b, mod_b_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&mod_b);

udynlink_link_dependency(&mod_a, &mod_b);
```

### Stage 2: Mixed Normal + Deferred Loads

Some modules load normally (all deps already satisfied), some deferred:

```c
// Provider loads normally — all its deps are already loaded
udynlink_load_module(&provider, provider_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&provider);

// Consumer depends on provider (already loaded) and on an optional logging module
// Provider links immediately. Logging is deferred.
udynlink_load_module(&consumer, consumer_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&consumer);

// Later, logging is loaded and linked
udynlink_load_module(&logging, logging_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
host_register(&logging);
udynlink_link_dependency(&consumer, &logging);
```

### Stage 3: Module-Level Optional Dependency Graceful Degradation

```c
// Inside a module
extern void log_printf(const char *fmt, ...);

void module_init(void) {
    if (log_printf != NULL) {
        log_printf("module init\n");
    } else {
        // Degraded mode: no logging
    }
}
```

---

## Task Breakdown

### Phase 0: Detection & Rejection (COMPLETE)

| Task | Status | Commit |
|------|--------|--------|
| Add `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` | ✅ Done | Already in tree |
| Add `udynlink_external_is_module_loading()` callback | ✅ Done | Already in tree |
| Wire checks into memory + streaming load paths | ✅ Done | Already in tree |
| Update test host with loading-state tracking | ✅ Done | Already in tree |
| Add self-dependency detection | ✅ Done | Already in tree |
| Create `test-circular-deps` integration test | ✅ Done | Already in tree |
| Update documentation | ✅ Done | Already in tree |

### Phase 1: Sentinel + link_dependency

| Task | Description | Agent | Notes |
|------|-------------|-------|-------|
| **1.1** | Add `UDYNLINK_DEP_DEFERRED` sentinel to `udynlink.h` | `quick` | One line macro |
| **1.2** | Add `UDYNLINK_SYM_DEFERRED` sentinel to `udynlink.h` | `quick` | One line macro |
| **1.3** | Modify `udynlink_load_module()` dep loop to handle `UDYNLINK_DEP_DEFERRED` | `quick` | ~5 lines: check `== UDYNLINK_DEP_DEFERRED`, `continue` |
| **1.4** | Modify `udynlink_load_module_from_stream()` dep loop to handle `UDYNLINK_DEP_DEFERRED` | `quick` | Mirror of 1.3 in streaming path |
| **1.5** | Modify relocation loop to handle `UDYNLINK_SYM_DEFERRED` from resolve callbacks | `quick` | In EXTERN resolution: if result == `UDYNLINK_SYM_DEFERRED`, write 0 and continue |
| **1.6** | Implement `udynlink_link_dependency()` in `udynlink.c` | `agent` | Symmetric dep linking + full EXTERN re-resolution (re-attempts deferred symbols) |
| **1.6a** | Implement `udynlink_link_symbol()` in `udynlink.c` | `agent` | Scan relocation table for `sym_name`, patch matching slots with `sym_addr`. ~50 lines. |
| **1.7** | Implement query helpers: `is_module_fully_linked()`, `get_linked_dependency()`, `is_symbol_resolved()` | `quick` | ~30 lines each, pure read-only |
| **1.8** | Add public declarations to `udynlink.h` | `quick` | `link_dependency` + `link_symbol` + 3 query helpers + 2 sentinels |
| **1.9** | Update `udynlink_error_msg()` for any new error codes | `quick` | If `link_dependency` adds new errors |
| **1.10** | Update test host to support deferred-return in callbacks | `quick` | `g_loading_names[]` + optional `resolve_symbol` deferred logic |
| **1.11** | Create `test-circular-deps-link` integration test | `agent` | A↔B circular pair via deferred + link_dependency; all load modes, -O3/-Os |
| **1.12** | Create `test-optional-dep` integration test | `agent` | Module with optional logging dep; verify graceful degradation then linking |
| **1.13** | Create `test-deferred-symbol` integration test | `agent` | Host returns `UDYNLINK_SYM_DEFERRED` for a symbol; module works with NULL; later re-resolved via link_dependency |
| **1.14** | Create `test-link-symbol` integration test | `agent` | Load module with deferred symbol; use `udynlink_link_symbol()` to patch it; verify module works |
| **1.15** | CI verification | `agent` | All existing tests pass + new tests pass |

### Phase 2: Documentation

| Task | Description | Agent | Notes |
|------|-------------|-------|-------|
| **2.1** | Document `UDYNLINK_DEP_DEFERRED` in `docs/integrating-as-host.md` | `quick` | Show callback returning the sentinel |
| **2.2** | Document `udynlink_link_dependency()` in `docs/api-reference.md` | `quick` | Semantics, symmetric behavior, re-resolution |
| **2.3** | Document query helpers in `docs/api-reference.md` | `quick` | `is_fully_linked`, `get_linked_dependency`, `is_symbol_resolved` |
| **2.4** | Document optional dependency pattern in `docs/how-it-works.md` | `quick` | Module checks `if (func_ptr)` pattern + caveats |
| **2.5** | Document `UDYNLINK_SYM_DEFERRED` in `docs/integrating-as-host.md` | `quick` | Show resolve callbacks returning the symbol sentinel |
| **2.5a** | Document `udynlink_link_symbol()` in `docs/api-reference.md` | `quick` | Semantics: direct relocation patch, no refcount/dep changes |
| **2.6** | Update `AGENTS.md` roadmap | `quick` | Mark sentinel + link_dependency as in-progress |

---

## Critical Design Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Sentinel `(udynlink_module_t*)1` vs flags parameter | **Sentinel** is simpler than changing `get_module_handle` signature or adding a new callback. It reuses the existing contract with one new valid return value. |
| D2 | `link_dependency()` symmetric vs directional | **Symmetric** — checks both directions. More ergonomic for circular pairs (one call links both). For directional-only linking, the host can check `get_linked_dependency()` first. |
| D3 | Re-resolve all EXTERNs on link | **Full re-scan** ensures tier 2 (dependency modules) takes precedence over tier 3 (host fallback) after linking. Correct and negligible overhead. |
| D4 | Query helpers in loader vs host | **In loader** — they inspect `p_mod->deps[]` and `p_header`, which are loader structures. Host shouldn't reach into module internals. |
| D5 | Optional dep detection for modules | **NULL check on function pointer** — if the host doesn't provide the symbol and the dep is deferred, the resolved value is 0. Module code checks `if (func)`. No new mechanism needed. **Caveat**: fails if host provides a stub. Document clearly. |
| D6 | Error codes for `link_dependency` | `UDYNLINK_ERR_LOAD_MISSING_DEP` if the dep is not found during link. `UDYNLINK_OK` if already linked or unrelated. No new error codes needed. |
| D7 | `UDYNLINK_SYM_DEFERRED` for individual symbols | **Yes** — same pattern as dependency deferral. Host can defer specific symbols without failing the whole load. Applied to both critical and fallback resolve callbacks. |
| D8 | Critical symbol deferral | **Allowed but discouraged** — if `udynlink_external_resolve_critical_symbol()` returns `UDYNLINK_SYM_DEFERRED`, the load continues. Hosts should only do this for truly deferrable critical symbols. |
| D9 | `link_symbol()` vs `link_dependency()` | **`link_symbol()` is a low-level patch** that writes directly to relocation slots. It does not touch refcounts, deps, or the three-tier chain. Use it for host-mediated symbol injection. **`link_dependency()` is high-level** and re-runs the resolution chain. Use it for module-to-module linking. Both can coexist. |

---

## Context Guide for Implementation Agents

### Existing Code to Leverage

| File | What to Reuse |
|------|---------------|
| `udynlink/udynlink.c` | Dependency validation loop in `udynlink_load_module()` (lines ~305-350) — copy and adapt for `link_dependency()` |
| `udynlink/udynlink.c` | Relocation loop for EXTERN symbols (lines ~426-450) — re-run this in `link_dependency()` for full re-resolution |
| `udynlink/udynlink.c` | `get_deps_strtab()`, `get_deps_strtab_offset()` — already locate dep names in the header |
| `tests/qemu_host/src/main.c` | `g_modules[]` registry pattern — add `g_loading_names[]` for deferred-return testing |

### `link_dependency()` Implementation Sketch

```c
udynlink_error_t udynlink_link_dependency(udynlink_module_t *a, udynlink_module_t *b) {
    if (!a || !b) return UDYNLINK_ERR_INVALID_ARGUMENT;

    const udynlink_module_header_t *ha = a->p_header;
    const udynlink_module_header_t *hb = b->p_header;

    // Direction A -> B
    const char *deps_a = get_deps_strtab(ha);
    for (uint16_t d = 0; d < ha->num_deps; d++) {
        if (strcmp(deps_a, udynlink_get_module_name(b)) == 0) {
            // Check if already linked
            int already = 0;
            for (uint16_t i = 0; i < a->num_deps; i++) {
                if (a->deps[i] == b) { already = 1; break; }
            }
            if (!already) {
                a->deps[a->num_deps++] = b;
                b->dep_refcount++;
                // Re-resolve EXTERNs in A
                apply_extern_relocations(a, ha);
            }
            break;
        }
        deps_a += strlen(deps_a) + 1;
    }

    // Direction B -> A (same pattern)
    // ...

    return UDYNLINK_OK;
}
```

### Query Helpers Implementation Sketch

```c
int udynlink_is_module_fully_linked(const udynlink_module_t *p_mod) {
    if (!p_mod || !p_mod->p_header) return 0;
    return p_mod->num_deps == p_mod->p_header->num_deps;
}

udynlink_module_t *udynlink_get_linked_dependency(const udynlink_module_t *p_mod, const char *dep_name) {
    if (!p_mod || !dep_name) return NULL;
    for (uint16_t i = 0; i < p_mod->num_deps; i++) {
        const char *name = udynlink_get_module_name(p_mod->deps[i]);
        if (name && strcmp(name, dep_name) == 0)
            return p_mod->deps[i];
    }
    return NULL;
}

int udynlink_is_symbol_resolved(const udynlink_module_t *p_mod, const char *sym_name) {
    if (!p_mod || !sym_name) return 0;
    udynlink_sym_t sym;
    if (!udynlink_lookup_symbol(p_mod, sym_name, &sym)) return 0;
    return sym.val != 0;
}
```

### Testing the New Path

**`test-circular-deps-link` test should**:
1. Build `mod_a.c` with `--depends mod_b`
2. Build `mod_b.c` with `--depends mod_a`
3. In `test_qemu.c`:
   - Pre-register both as "loading" in the host
   - Load A (B is deferred)
   - Load B (A is loaded, so linked immediately)
   - Verify B has A in deps, A does not have B
   - Call `udynlink_link_dependency(&mod_a, &mod_b)`
   - Verify A now has B in deps, both have dep_refcount == 1
   - Call `run_test_func()` on both modules (call exported functions that reference each other)
   - Verify `udynlink_unload_module()` fails for both (circular refcount trap)
   - Print `*** TEST OK ***`

**`test-optional-dep` test should**:
1. Build `mod_consumer.c` with `--depends logging`
2. Build `mod_logging.c` (optional provider)
3. In `test_qemu.c`:
   - Load consumer (logging is deferred; host callback returns `UDYNLINK_DEP_DEFERRED`)
   - Verify consumer's `log_printf` extern is 0
   - Call consumer's init function — it should check `if (log_printf)` and not crash
   - Load logging module normally
   - Call `udynlink_link_dependency(&consumer, &logging)`
   - Verify consumer's `log_printf` is now non-zero
   - Call consumer's init function again — logging should work
   - Print `*** TEST OK ***`

**`test-deferred-symbol` test should**:
1. Build `mod_defer_sym.c` with an `extern void late_init(void);` symbol (no `--depends`, pure host symbol)
2. In `test_qemu.c`:
   - Load `mod_defer_sym` with the host returning `UDYNLINK_SYM_DEFERRED` for `late_init`
   - Verify load succeeds (does not fail)
   - Call module's test function — it should check `if (late_init)` and not crash
   - Host now provides the real `late_init` symbol
   - Call `udynlink_link_dependency(&mod_defer_sym, NULL)` or trigger re-resolution somehow
   - Verify `late_init` is now resolved
   - Call module's test function again — it should call `late_init` successfully
   - Print `*** TEST OK ***`

**`test-link-symbol` test should**:
1. Build `mod_link_sym.c` with an `extern void my_service(void);` symbol
2. In `test_qemu.c`:
   - Load `mod_link_sym` with the host returning `0` for `my_service` (not found, but module handles it)
   - Verify module's `my_service` is 0
   - Call `udynlink_link_symbol(&mod, "my_service", (uint32_t)(uintptr_t)&mock_service)`
   - Verify module's `my_service` now points to `mock_service`
   - Call module's test function — it should call `mock_service`
   - Print `*** TEST OK ***`

### `link_symbol()` Implementation Sketch

```c
udynlink_error_t udynlink_link_symbol(udynlink_module_t *p_mod, const char *sym_name, uint32_t sym_addr) {
    if (!p_mod || !sym_name || !p_mod->p_header) return UDYNLINK_ERR_INVALID_ARGUMENT;

    const udynlink_module_header_t *p_header = p_mod->p_header;
    int found = 0;

    for (uint16_t r = 0; r < p_header->num_relocs; r++) {
        uint32_t rel_offset;
        const char *p_reloc = get_reloc_at(p_header, r, &rel_offset);
        if (p_reloc == NULL) continue;

        udynlink_sym_t sym;
        if (get_sym_at(p_header, rel_offset, &sym) && sym.name && strcmp(sym.name, sym_name) == 0) {
            uint8_t *p_rel_location = p_mod->ram_base + rel_offset;
            // Write the address directly to the relocation slot
            // Handle different relocation types if needed
            *(uint32_t *)p_rel_location = sym_addr;
            found = 1;
        }
    }

    return found ? UDYNLINK_OK : UDYNLINK_ERR_LOAD_CANT_RESOLVE;
}
```

### Pitfalls

1. **Do not change `udynlink_load_module()` behavior for non-deferred paths.** The only change is the `== UDYNLINK_DEP_DEFERRED` check inside the dep loop.
2. **`link_dependency()` must not double-increment refcounts.** Always check `deps[]` before adding.
3. **Re-resolution in `link_dependency()` must handle all relocation types** (LOT, data, etc.), not just function pointers. Re-run the same relocation loop logic but only for EXTERN symbols.
4. **The streaming path must also handle the sentinel.** Both memory and stream loaders use `udynlink_external_get_module_handle()`.
5. **Sentinel safety**: On 64-bit hosts (not Cortex-M), `(void*)1` is still a valid non-NULL, non-aligned pointer. udynlink targets ARM Cortex-M (32-bit), so this is safe. Document this constraint.
6. **Symbol sentinel safety**: `UDYNLINK_SYM_DEFERRED` is `(uint32_t)1`. On Cortex-M, address `0x00000001` is not valid for code or data, so it can never be confused with a real symbol address. Document this.
7. **Optional dep caveats**: If the host provides a fallback stub for a deferred symbol, the module's `if (func != NULL)` check will falsely succeed. Document that hosts should not provide stubs for optional symbols if they want modules to detect absence.
8. **Symbol deferral in critical callback**: If `udynlink_external_resolve_critical_symbol()` returns `UDYNLINK_SYM_DEFERRED`, the load continues. This weakens the "critical" semantics. Document that this should only be done intentionally for deferrable critical symbols. Hosts that never return the sentinel maintain the old strict critical-symbol behavior.
9. **`link_symbol()` does not validate the address.** The host is responsible for passing a valid address. Passing `0` or a misaligned address will crash when the module calls the symbol.
10. **`link_symbol()` does not update the symbol table.** It patches the relocation slot directly. The symbol table (`symtab`) still shows the old (or 0) value. This is intentional — the relocation slot is what the running code uses, not the symbol table. If the host needs the symbol table to reflect the new value, it should use `udynlink_lookup_symbol()` and update it manually (not recommended).

---

## Open Questions

1. **Should `link_dependency()` return a count of how many directions were actually linked?** This would let the host know if anything happened. **Recommendation**: No — return `UDYNLINK_OK` always. The host can query with `is_module_fully_linked()` if it needs to know.
2. **Should we add `udynlink_unlink_dependency()` to reverse a link without unloading?** This would decrement refcounts and remove from `deps[]`. **Recommendation**: Defer to future plan. Not needed for initial support.
3. **Should `udynlink_link_dependency()` also work for modules that were loaded normally (not deferred)?** Yes — it is idempotent. If A already has B linked, it returns OK. This lets the host call it defensively.

