# Plan: Backwards-Compatible Circular Dependency Support

> **Status**: Phase 0 (detection & rejection) is **COMPLETE**. This plan describes the opt-in extension for hosts that need to load circular dependency graphs.

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

Provide an **opt-in, backwards-compatible** two-phase loading path for advanced use cases where modules genuinely need mutual references. The default `udynlink_load_module()` remains strict.

## Design Principles

1. **Never break existing behavior.** `udynlink_load_module()` keeps rejecting cycles.
2. **Opt-in via new API.** Two-phase loading is a separate code path that hosts must explicitly choose.
3. **Loader stays stateless.** The host orchestrates load order and tracks state.
4. **Refcount semantics are unchanged.** Circular deps still create un-unloadable modules. Document this clearly.
5. **Minimal API surface.** Only add what's strictly necessary.

## Proposed API

### Already Implemented (Phase 0)

```c
uint32_t udynlink_get_module_deps(const void *base_addr, const char **deps, uint32_t max_deps);
```

Hosts can use this today to inspect module headers before loading, build dependency graphs, and plan load order.

### Phase 1: Two-Phase Loading (Deferred + Link)

#### `udynlink_load_module_deferred()`

```c
udynlink_error_t udynlink_load_module_deferred(
    udynlink_module_t *p_mod,
    const void *base_addr,
    void *load_addr,
    uint32_t load_size,
    udynlink_load_mode_t load_mode
);
```

**Behavior**: Identical to `udynlink_load_module()` **except**:
- **No dependency validation** — does not call `udynlink_external_get_module_handle()`.
- **No extern symbol resolution** — for `UDYNLINK_SYM_TYPE_EXTERN` relocations, writes `0` as a placeholder instead of resolving.
- Sets `p_mod->num_deps = 0` and does not populate `p_mod->deps[]`.
- Does not increment any `dep_refcount`.
- Still performs: signature/version/arch validation, RAM allocation, section copying, BSS zeroing, internal relocations.

**Error handling**: On failure, cleanup is identical to normal load (free RAM, zero handle).

#### `udynlink_link_module()`

```c
udynlink_error_t udynlink_link_module(udynlink_module_t *p_mod);
```

**Behavior**:
- Reads dependency names from `p_mod->p_header`.
- For each dependency name, calls `udynlink_external_get_module_handle(name)`.
  - If NULL → `UDYNLINK_ERR_LOAD_MISSING_DEP`.
  - If found → stores handle in `p_mod->deps[]`, increments `dep_mod->dep_refcount`, increments `p_mod->num_deps`.
- Re-scans the relocation table and resolves `UDYNLINK_SYM_TYPE_EXTERN` symbols.
  - Resolution order: critical host → dependency modules → fallback host.
- **Idempotent**: If `p_mod->num_deps > 0` (already linked), returns `UDYNLINK_OK` immediately. This allows hosts to call `link_module()` on a mixed set of deferred and normally-loaded modules.

#### Streaming Variant

```c
udynlink_error_t udynlink_load_module_from_stream_deferred(
    udynlink_module_t *p_mod,
    const udynlink_io_t *p_io,
    void *load_addr,
    uint32_t load_size,
    udynlink_load_mode_t load_mode,
    void *work_buf,
    uint32_t work_buf_size
);
```

Same deferred semantics as the memory-path variant.

### Phase 2: Convenience Helpers (Optional)

#### `udynlink_analyze_deps()`

```c
udynlink_error_t udynlink_analyze_deps(
    const void **base_addrs,
    uint32_t count,
    uint32_t *load_order_out,
    uint32_t *load_order_count_out
);
```

Stack-only topological sort / cycle detection using `udynlink_get_module_deps()`. Returns `UDYNLINK_OK` with a load order for acyclic graphs, or `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` if a cycle is found. Pure helper — does not load anything.

**Why this is Phase 2**: Hosts can build their own graph logic. This is a nice-to-have for portability.

---

## Backwards Compatibility Analysis

| Aspect | Before | After Phase 1 | After Phase 2 |
|--------|--------|---------------|---------------|
| `udynlink_load_module()` | Rejects cycles | **Unchanged** — still rejects cycles | **Unchanged** |
| `udynlink_load_module_from_stream()` | Rejects cycles | **Unchanged** — still rejects cycles | **Unchanged** |
| Error codes | `MISSING_DEP` for all missing deps | `CIRCULAR_DEP` for detected cycles (already done) | **Unchanged** |
| Host callbacks | 7 required | **Unchanged** — `is_module_loading` is still optional/weak | **Unchanged** |
| `dep_refcount` semantics | Incremented on load | **Unchanged** — deferred load skips it; `link_module()` adds it | **Unchanged** |
| Existing tests | All pass | All pass + new deferred-load tests | All pass + new analyzer tests |

**Guarantee**: No existing host firmware needs to change. The new APIs are strictly additive.

---

## Migration Path for Hosts

### Stage 0: Today (No Changes Needed)

Use `udynlink_load_module()` as always. Circular deps are rejected. If you need to plan load order, use `udynlink_get_module_deps()` to inspect module headers first.

### Stage 1: Adopting Two-Phase Loading (Opt-In)

For a circular pair `mod_a ↔ mod_b`:

```c
// 1. Deferred load both (no dep checks, no extern resolution)
udynlink_load_module_deferred(&mod_a, mod_a_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
udynlink_host_register_module(&mod_a);  // make findable by get_module_handle

udynlink_load_module_deferred(&mod_b, mod_b_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
udynlink_host_register_module(&mod_b);  // make findable by get_module_handle

// 2. Link both (resolves deps and extern symbols)
udynlink_link_module(&mod_a);
udynlink_link_module(&mod_b);
```

**Important**: Both modules must be deferred-loaded **and** registered in the host's module table before either is linked. If `mod_a` is linked before `mod_b` is registered, `get_module_handle("mod_b")` will return NULL and linking fails.

### Stage 2: Mixed Normal + Deferred Loads

You can load some modules normally and some deferred in the same system, as long as dependency directionality is respected:

```c
// Normal load: provider must not depend on deferred modules
udynlink_load_module(&provider, provider_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
udynlink_host_register_module(&provider);

// Deferred load: consumer can depend on provider (already loaded)
// But if consumer also has a circular dep on another deferred module,
// that other module must also be deferred-loaded and registered first.
```

### Stage 3: Using `udynlink_analyze_deps()` (If Implemented)

```c
const void *images[] = {mod_a_image, mod_b_image, mod_c_image};
uint32_t order[3];
uint32_t order_count;

udynlink_error_t err = udynlink_analyze_deps(images, 3, order, &order_count);
if (err == UDYNLINK_ERR_LOAD_CIRCULAR_DEP) {
    // Graph has a cycle. Decide: reject, or use deferred loading.
}

// Load in the returned order (acyclic case)
for (uint32_t i = 0; i < order_count; i++) {
    udynlink_load_module(&mods[i], images[order[i]], NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
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

### Phase 1: Two-Phase Loading API

| Task | Description | Agent | Notes |
|------|-------------|-------|-------|
| **1.1** | Refactor shared load logic into static helpers | `agent` | Extract validation, RAM alloc, copy, internal relocations so `load_module` and `load_module_deferred` share code |
| **1.2** | Implement `udynlink_load_module_deferred()` | `agent` | Uses shared helpers; skips dep validation and extern resolution |
| **1.3** | Implement `udynlink_link_module()` | `agent` | Dep validation + extern resolution; idempotent for already-linked modules |
| **1.4** | Implement `udynlink_load_module_from_stream_deferred()` | `agent` | Mirrors streaming path with deferred semantics |
| **1.5** | Add public declarations to `udynlink.h` | `quick` | All 3 new functions |
| **1.6** | Add deferred-load wrappers to test host | `quick` | `test_load_module_deferred()` that registers in host table |
| **1.7** | Create `test-circular-deps-deferred` integration test | `agent` | Load circular A↔B via deferred + link; verify both modules work; all load modes, -O3/-Os |
| **1.8** | CI verification | `agent` | All existing tests pass + new deferred test passes |

### Phase 2: Convenience Helpers (Optional)

| Task | Description | Agent | Notes |
|------|-------------|-------|-------|
| **2.1** | Implement `udynlink_analyze_deps()` | `agent` | Stack-only Kahn's sort. ~150 lines. Pure helper. |
| **2.2** | Add test for `udynlink_analyze_deps()` | `agent` | Verify topological order and cycle detection |
| **2.3** | Update documentation with `analyze_deps` usage | `quick` | Add to docs/how-it-works.md and api-reference.md |

### Phase 3: Documentation Updates

| Task | Description | Agent | Notes |
|------|-------------|-------|-------|
| **3.1** | Document two-phase loading in `docs/how-it-works.md` | `quick` | Show the deferred → register → link pattern |
| **3.2** | Document `udynlink_load_module_deferred()` in `docs/api-reference.md` | `quick` | Semantics, parameters, error codes |
| **3.3** | Document `udynlink_link_module()` in `docs/api-reference.md` | `quick` | Semantics, idempotency, refcount side effects |
| **3.4** | Document `udynlink_load_module_from_stream_deferred()` in `docs/api-reference.md` | `quick` | Streaming variant |
| **3.5** | Add migration guide to `docs/integrating-as-host.md` | `quick` | Stage 0 → Stage 1 → Stage 2 |
| **3.6** | Update `AGENTS.md` roadmap | `quick` | Mark two-phase loading as in-progress |

---

## Critical Design Decisions

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Separate functions vs flags parameter | **Separate functions** (`_deferred` suffix). Keeps existing API signatures stable. No risk of breaking ABI or existing callers. |
| D2 | `link_module()` idempotency | **Return `UDYNLINK_OK` for already-linked modules**. Hosts may call it on mixed sets of deferred and normal modules. |
| D3 | Extern placeholder in deferred load | **Write `0`**. Safer than leaving garbage; signals "not yet linked" if accidentally called. |
| D4 | Refcount in deferred load | **Do not increment**. Deferred modules are "invisible" as dependencies until linked. This allows hosts to abort a batch without leaving dangling refcounts. |
| D5 | Streaming deferred variant | **Add as separate function**. Matches existing pattern (`load_module` / `load_module_from_stream`). |
| D6 | `analyze_deps()` scope | **Phase 2, optional**. Not required for correctness. Hosts can build their own logic. |

---

## Context Guide for Implementation Agents

### Existing Code to Leverage

| File | What to Reuse |
|------|---------------|
| `udynlink/udynlink.c` | `get_deps_strtab()`, `get_deps_strtab_offset()`, `get_code_offset_from_header()`, `get_header_size()` — already compute header layout correctly |
| `udynlink/udynlink.c` | Relocation loop pattern in `udynlink_load_module()` (lines ~370-450) — copy and filter for EXTERN-only in `link_module()` |
| `udynlink/udynlink.c` | Dependency validation loop (lines ~305-350) — reuse essentially verbatim in `link_module()` |
| `tests/qemu_host/src/main.c` | `g_modules[]` registry pattern — `test_load_module_deferred()` should register immediately so `link_module()` can find deps |

### Refactoring Strategy for Shared Load Logic

The current `udynlink_load_module()` is a single long function. Extract these static helpers:

```c
// 1. Header validation (signature, version, arch, bounds)
static udynlink_error_t validate_header(const udynlink_module_header_t *p_header);

// 2. RAM allocation + ownership flag setup
static udynlink_error_t setup_ram(udynlink_module_t *p_mod, const udynlink_module_header_t *p_header, void *load_addr, uint32_t load_size, udynlink_load_mode_t load_mode);

// 3. Copy header/code/data based on load mode
static void copy_module_sections(udynlink_module_t *p_mod, const void *base_addr, const udynlink_module_header_t *p_header, udynlink_load_mode_t load_mode);

// 4. Relocation loop with a symbol-type filter
// 'resolve_extern' = false for deferred load (write 0 for EXTERN)
// 'resolve_extern' = true for normal load (full resolution)
static udynlink_error_t apply_relocations(udynlink_module_t *p_mod, const udynlink_module_header_t *p_header, int resolve_extern);
```

Then:
- `udynlink_load_module()` = `validate_header` → `setup_ram` → `copy_sections` → `apply_relocations(p_mod, p_header, 1)` → dep validation
- `udynlink_load_module_deferred()` = `validate_header` → `setup_ram` → `copy_sections` → `apply_relocations(p_mod, p_header, 0)` → skip dep validation
- `udynlink_link_module()` = dep validation → `apply_relocations(p_mod, p_header, 1)` (but only for EXTERN symbols; internal/exported already done)

**Important**: `apply_relocations` with `resolve_extern=0` must still write a placeholder (0) to EXTERN relocation slots so the memory is in a known state.

### Testing the Deferred Path

The `test-circular-deps-deferred` test should:
1. Build `mod_a.c` with `--depends mod_b`
2. Build `mod_b.c` with `--depends mod_a`
3. In `test_qemu.c`:
   - Deferred load A, register in host table
   - Deferred load B, register in host table
   - Link A, link B
   - Verify `run_test_func()` works on both modules (call exported functions)
   - Verify `dep_refcount` on both is 1
   - Verify `udynlink_unload_module()` fails with `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` for both
   - Print `*** TEST OK ***`

### Pitfalls

1. **Do not change `udynlink_load_module()` behavior.** The existing strict path must remain untouched except for the static helper extraction.
2. **`link_module()` must not re-apply internal/exported relocations.** Those were already done in deferred load. Only scan for EXTERN symbols.
3. **The test host's `udynlink_external_get_module_handle()` must find deferred-loaded modules.** `test_load_module_deferred()` should register in `g_modules[]` immediately after deferred load succeeds, just like `test_load_module()` does after normal load.
4. **Placeholder 0 for EXTERN relocations**: If a deferred-loaded module is accidentally called before linking, it will dereference a null function pointer (LOT entry = 0). This is a clean crash, not a jump to random memory. Acceptable for a power-user API.

---

## Open Questions

1. **Should `udynlink_link_module()` emit a debug log when it skips an already-linked module?** Low priority — a single `UDYNLINK_DEBUG_INFO` line is fine.
2. **Should we add a `udynlink_unlink_module()` to reverse `link_module()` without unloading?** This would enable breaking cycles by decrementing refcounts and zeroing deps. Could be useful for graceful teardown. **Recommendation**: Defer to a future plan. Not needed for initial two-phase support.
3. **Should `udynlink_analyze_deps()` also report the cycle path?** (e.g., which modules form the cycle). **Recommendation**: No — keep the helper minimal. Returning the error code is sufficient; hosts can do their own DFS if they need the exact path.

