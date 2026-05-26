# Plan: Circular Dependency Support for udynlink

## Current State Summary

The udynlink loader (ABI v2.0+) supports module dependency tracking:

- **Declaration**: `mkmodule --depends mod_a,mod_b` encodes dependency names in the module header's `deps_strtab`.
- **Runtime validation**: During `udynlink_load_module()`, the loader iterates over `num_deps` names and calls `udynlink_external_get_module_handle(name)`. If any dependency is not already loaded, the load fails with `UDYNLINK_ERR_LOAD_MISSING_DEP`.
- **Reference counting**: On successful load, `dep_mod->dep_refcount++` is incremented. `udynlink_unload_module()` rejects unloading if `dep_refcount > 0`, and decrements refcounts of its own dependencies.
- **Three-tier symbol resolution**: Critical host → dependency modules → fallback host.

**Key constraint**: The loader has **no global state** and no module table of its own. It relies entirely on host callbacks (`udynlink_external_get_module_handle`) to discover already-loaded modules.

## Revised Goal

**Allow circular dependencies to load successfully**, rather than rejecting them. The loader remains stateless; the host orchestrates load order. We provide:

1. A **two-phase loading API** so the host can load all modules in a cycle first, then resolve their dependencies and extern symbols in a second pass.
2. **Helper functions** to inspect a module's dependencies before loading, so the host can plan or detect cycles if it wants to.
3. **Clear documentation** of the refcount implications of circular dependencies.

## Why Not Reject Cycles?

In embedded systems, modules may legitimately need mutual interaction (e.g., a protocol stack module and a HAL module that call each other's exported functions). The cleanest architectural fix is a host-mediated pubsub channel, but udynlink should not prevent users from using direct mutual references if they choose to. The loader's job is to load safely; the host's job is to manage lifecycle.

---

## Proposed API Additions

### 1. `udynlink_load_module_deferred()` — Phase 1 Load

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
- Skips dependency validation (does not call `udynlink_external_get_module_handle`).
- Skips extern symbol resolution in the relocation loop.
- Still performs all other steps: signature/version/arch checks, RAM allocation, section copying, BSS zeroing, **internal** relocations (internal/exported symbols only), and relocation-table bounds validation.
- Sets `p_mod->num_deps = 0` and does not populate `p_mod->deps[]`.
- Does not increment any `dep_refcount`.

**Error handling**: On failure, cleanup is identical to normal load (free RAM, zero handle).

### 2. `udynlink_link_module()` — Phase 2 Link

```c
udynlink_error_t udynlink_link_module(udynlink_module_t *p_mod);
```

**Behavior**:
- Reads `num_deps` and dependency names from `p_mod->p_header`.
- For each dependency name, calls `udynlink_external_get_module_handle(name)`.
  - If NULL → `UDYNLINK_ERR_LOAD_MISSING_DEP` (dependency was not loaded in Phase 1).
  - If found → stores handle in `p_mod->deps[]`, increments `dep_mod->dep_refcount`, increments `p_mod->num_deps`.
- Re-runs the relocation loop (or re-scans only extern relocations) to resolve `UDYNLINK_SYM_TYPE_EXTERN` symbols.
  - Resolution order: critical host → dependency modules → fallback host.
  - Dependency modules are now guaranteed to be loaded, so their exported symbols are available.

**Important**: A module may be linked multiple times? No — linking should be idempotent or guarded. Better: linking should fail if `num_deps > 0` already (module already linked). Add a check: if `p_mod->num_deps > 0`, return `UDYNLINK_ERR_INVALID_MODULE` or `UDYNLINK_OK` (already linked). Decision needed.

> **Recommendation**: Return `UDYNLINK_OK` if already linked, or `UDYNLINK_ERR_INVALID_MODULE` to catch misuse. The safer choice is `UDYNLINK_OK` (idempotent) since a host might call link on all modules in a loop, and some may already be linked from a previous batch.

Actually, no — if a module was loaded normally (not deferred), it's already linked. A host that loads some normally and some deferred, then calls `udynlink_link_module()` on all of them, should not get an error for the normally-loaded ones. So `link_module()` should return `UDYNLINK_OK` for already-linked modules (or simply skip them).

Wait, but `udynlink_load_module()` already does everything including linking. We don't want `link_module()` to re-resolve externs for normally-loaded modules (that would be harmless but redundant). Simpler: `link_module()` only works on modules where `num_deps == 0` and the header says `num_deps > 0` (deferred load). If `num_deps > 0` already, return `UDYNLINK_OK` with a debug log. If `num_deps == 0` and header says `num_deps == 0`, also return `UDYNLINK_OK` (no deps to link).

### 3. `udynlink_get_module_deps()` — Pre-Load Inspection Helper

```c
// Reads dependency names from a module image without loading it.
// Returns the number of dependencies (0 if none, v1.0 module, or error).
// Writes up to max_deps pointers into the deps array.
// The returned pointers point into the module image's string table and are valid as long as base_addr is valid.
uint32_t udynlink_get_module_deps(const void *base_addr, const char **deps, uint32_t max_deps);
```

**Use case**: Host reads all module headers, builds a dependency graph, detects cycles if desired, and decides load order. The loader remains stateless; the helper is pure read-only header parsing.

### 4. Streaming Variant

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

Same deferred semantics: skip dep validation and extern resolution. Still copies data, applies internal relocations, zeros BSS.

### 5. Optional: `udynlink_analyze_deps()` — Host Convenience

```c
// Given an array of module base addresses, inspect each header and build a dependency graph.
// If load_order_out is non-NULL, fills it with a topological sort order (indices into base_addrs).
// If a cycle is detected, returns UDYNLINK_ERR_LOAD_CIRCULAR_DEP and load_order_out is undefined.
// This is a pure helper; it does not load anything.
udynlink_error_t udynlink_analyze_deps(
    const void **base_addrs,
    uint32_t count,
    uint32_t *load_order_out,
    uint32_t *load_order_count_out
);
```

**Algorithm**: Stack-only Kahn's topological sort or DFS. Uses `udynlink_get_module_deps()` to read edges. Bounded by `count * UDYNLINK_MAX_DEPS` edges. No heap allocation. Returns `UDYNLINK_OK` for acyclic graphs, `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` if cycle found.

**Note**: This is strictly optional. The host can implement its own graph logic. But providing a small, correct, stack-only implementation improves portability and reduces host-side bugs.

---

## Decision Points

| # | Decision | Options | Recommendation |
|---|----------|---------|----------------|
| D1 | `link_module()` idempotency | (a) Error if already linked, (b) OK/skip if already linked | **(b)** — host may call link on mixed set of deferred and normal modules |
| D2 | Streaming deferred variant | (a) New function, (b) Add flags parameter to existing function | **(a)** — keeps existing API stable, new function mirrors existing streaming API pattern |
| D3 | `udynlink_analyze_deps()` scope | (a) Include in plan, (b) Skip — let host do its own graph logic | **(a)** — small convenience helper, ~150 lines, improves embedded portability |
| D4 | Error code for cycle in analyzer | (a) Reuse `UDYNLINK_ERR_LOAD_CIRCULAR_DEP`, (b) New `UDYNLINK_ERR_CIRCULAR_DEP` | **(a)** — single error code is simpler. Add it to the enum even if the loader never emits it directly (analyzer does). |

---

## Task Breakdown

### Phase 1: Core Two-Phase Loading

| Task | Description | Agent | Deliverable |
|------|-------------|-------|-------------|
| **1.1** | Implement `udynlink_load_module_deferred()` in `udynlink.c` | `agent` | New function, ~60% duplicate of `udynlink_load_module()` — refactor shared logic into static helpers to avoid duplication |
| **1.2** | Implement `udynlink_link_module()` in `udynlink.c` | `agent` | New function: dep validation + extern resolution. Handles already-linked modules gracefully. |
| **1.3** | Implement `udynlink_load_module_from_stream_deferred()` in `udynlink.c` | `agent` | New function, mirrors streaming path with deferred semantics |
| **1.4** | Implement `udynlink_get_module_deps()` in `udynlink.c` | `quick` | Read-only helper, ~30 lines |
| **1.5** | Add new error code `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` to `udynlink.h` | `quick` | One line in error enum |
| **1.6** | Add public declarations to `udynlink.h` | `quick` | Declarations for all 4 new functions |
| **1.7** | Update test host (`tests/qemu_host/src/main.c`) to support deferred loading in registry | `quick` | Ensure deferred-loaded modules can be found via `udynlink_external_get_module_handle()` before linking |
| **1.8** | End-to-end test: new `test-circular-deps/` | `agent` | Test with circular A↔B and acyclic A→B→C graphs, all load modes, -O0 and -Os |

### Phase 2: Convenience Helper (Optional but Recommended)

| Task | Description | Agent | Deliverable |
|------|-------------|-------|-------------|
| **2.1** | Implement `udynlink_analyze_deps()` | `agent` | Stack-only topological sort / cycle detection. ~150 lines. |
| **2.2** | Test `udynlink_analyze_deps()` in QEMU test | `agent` | Verify cycle detection and topological order for known graphs |

### Phase 3: Documentation

| Task | Description | Agent | Deliverable |
|------|-------------|-------|-------------|
| **3.1** | Update README / docs with circular dep loading pattern | `quick` | Document: Phase 1 deferred → register → Phase 2 link. Document refcount trap. |
| **3.2** | Update `AGENTS.md` or codemap with new API | `quick` | Reference new functions and loading pattern |
| **3.3** | CI verification | `agent` | All existing tests pass + new circular dep test passes |

---

## Context Guide for Implementation Agents

### Key Files

| File | Relevance | Key Lines |
|------|-----------|-----------|
| `udynlink/udynlink.h` | Error codes (line 156), module struct (line 98), public API | Add declarations after existing load functions |
| `udynlink/udynlink.c` | Core loader | `udynlink_load_module()` (line 252), `udynlink_load_module_from_stream()` (line 606), dependency block (305-336), extern resolution (426-450) |
| `udynlink/udynlink_externals.h` | Host callbacks | No new callbacks needed! Existing `udynlink_external_get_module_handle` is sufficient. |
| `tests/qemu_host/src/main.c` | Test host registry | `g_modules[]` (line 20), `udynlink_external_get_module_handle()` (line 77) |
| `tests/test-deps/` | Existing dependency test | Reference for `--depends` test harness |

### Critical Design Constraints

1. **Do not add global state to the loader.** The deferred/link split keeps the loader stateless. The host decides when to link.
2. **Refactor, don't copy-paste.** `udynlink_load_module()` and `udynlink_load_module_deferred()` share ~80% of their logic (signature checks, RAM allocation, copy, internal relocations). Extract shared helpers:
   - `validate_and_setup_header(p_mod, p_header, load_mode)` — signature, version, arch, header field validation
   - `allocate_ram(p_mod, p_header, load_addr, load_size, load_mode)` — RAM size computation and allocation
   - `copy_sections(p_mod, p_header, base_addr, load_mode)` — memcpy logic for COPY_ALL / COPY_TEXT_DATA / XIP
   - `apply_internal_relocations(p_mod, p_header)` — relocation loop but only for INTERNAL/EXPORTED symbols
   - `resolve_extern_relocations(p_mod, p_header)` — relocation loop but only for EXTERN symbols (used by `link_module`)
   
   However, be careful: the relocation loop in `udynlink_load_module()` is a single loop that handles all symbol types. Splitting it cleanly might require iterating over the relocation table twice (once for internal/exported, once for extern). That's acceptable for deferred/link since `link_module` only needs to process externs.

   **Better approach**: Keep the relocation loop as-is but parameterize it with a callback or symbol-type filter. Or, simpler: in deferred load, process all relocations but for EXTERN symbols, skip resolution (leave LOT/data entry as 0 or unmodified). In `link_module()`, re-scan only EXTERN relocations and resolve them.

   Wait — can we skip extern relocations in deferred load? The relocation table says "at this lot_offset/data_offset, write the address of this extern symbol." If we skip it, the slot contains garbage (whatever was in the module image). We should probably write 0 as a placeholder, or leave it as-is (it will be overwritten in link). Leaving it as-is is fine if the host guarantees not to call module functions before linking. Writing 0 is safer.

   **Recommendation**: In deferred load, for EXTERN relocations, write `0` to `p_rel_location` and skip the resolution. This prevents accidental use of unlinked extern symbols. In `link_module()`, re-scan all relocations and for EXTERN symbols, perform full resolution.

3. **`link_module()` must handle the fact that `p_mod->p_header` might be in RAM (COPY_ALL mode) or flash (XIP / COPY_TEXT_DATA).** It only reads the header and relocation table, which is safe in all modes.

4. **Streaming deferred load**: Similar to memory path — read all metadata and data, apply internal relocations, write 0 for extern relocations. Skip dep validation.

5. **Test host registry**: The test host currently registers modules in `test_load_module()` *after* `udynlink_load_module()` succeeds. For deferred loading, the host test helper should provide a `test_load_module_deferred()` that also registers immediately after deferred load, so that `link_module()` can find dependencies.

### Example: Loading a Circular Pair

```c
// Host has discovered mod_a and mod_b images
// Both declare --depends on each other

udynlink_module_t mod_a, mod_b;

// Phase 1: Deferred load both
udynlink_load_module_deferred(&mod_a, mod_a_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
udynlink_test_register_module(&mod_a);  // make it findable
udynlink_load_module_deferred(&mod_b, mod_b_image, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL);
udynlink_test_register_module(&mod_b);  // make it findable

// Phase 2: Link both (order doesn't matter for exported-only mutual refs)
udynlink_link_module(&mod_a);
udynlink_link_module(&mod_b);

// Both are now fully operational
// NOTE: mod_a->dep_refcount == 1, mod_b->dep_refcount == 1
// Individual unload will fail until both are explicitly unloaded together
// (or a future cycle-break API is used)
```

### Refcount Implications

After linking a circular dependency:
- Every module in the cycle has `dep_refcount >= 1` (from at least one other module in the cycle).
- `udynlink_unload_module()` will return `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` for every module in the cycle.
- **This is by design.** The cycle represents a mutual obligation; none can be safely unloaded while the others remain because their symbol tables are referenced.
- **Host strategy**: If the host wants to tear down a circular group, it must either:
  1. Unload all modules in the group simultaneously (future API).
  2. Break the cycle first by having one module deregister its exported symbols (host-mediated).
  3. Use a refcount-weak dependency model (not implemented; would require distinguishing strong vs weak deps).

Document this clearly. Do not attempt to solve automatic circular unloading now.

---

## Open Questions

1. Should `udynlink_link_module()` write a debug log when it skips an already-linked module?
2. Should `udynlink_load_module_deferred()` accept a new `udynlink_load_mode_t` value, or should we add a separate `uint32_t flags` parameter to the load API? (Current plan: new function, no flags — simplest and most backward-compatible.)
3. Should `udynlink_analyze_deps()` be in Phase 1 (core) or Phase 2 (convenience)? Recommendation: Phase 2, since it's a nice-to-have and not required for correctness.
