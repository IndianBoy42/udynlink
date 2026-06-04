# Caller Tracking for Host Callbacks

## Problem

When a module calls a host callback (e.g. `printf` via its LOT), the host function has **no way to know which module invoked it**. This matters for per-module resource accounting, access control, debugging, and module-specific state routing.

The same problem exists at **load-time**: `udynlink_external_resolve_symbol(const char *name)` receives only the symbol name — the resolver cannot tell which module is being loaded.

## Solution: Optional global `udynlink_current_module`

A global pointer that tracks the "currently active" module. Set by:
- **Load-time**: Core loader sets it before calling the resolver during `udynlink_load_module_image()`, `udynlink_link_incremental()`, and `udynlink_relink_all()`
- **Runtime**: Host uses `_TRACKED` variants of the call macros to set/restore it around module calls

Callbacks read `udynlink_current_module` directly — no signature changes, no host data structure assumptions, no O(n) lookups.

### API

```c
// udynlink/udynlink_caller.h

/// The currently active module, or NULL.
/// - At load-time: set by the loader during symbol resolution
/// - At runtime: set by UDYNLINK_CALL_TRACKED and friends
/// - Callbacks may read but should not write this variable.
extern udynlink_module_t *udynlink_current_module;

/// Save/set/restore udynlink_current_module around a module call.
#define UDYNLINK_CALL_TRACKED(p_func, ret_type, args) ...
#define UDYNLINK_CALL_TRACKED_VOID(p_func, args) ...
#define UDYNLINK_CALL_MODULE_FUNC_TRACKED(p_mod, name, ret_type, args, p_out_ret) ...

/// Manual tracking: set udynlink_current_module + r9.
/// Caller must save/restore both manually (same contract as UDYNLINK_PREPARE_CALL).
#define UDYNLINK_PREPARE_CALL_TRACKED(p_mod) ...
```

### Scope

| Aspect | Behavior |
|--------|----------|
| **Load-time** | Loader saves old value, sets `udynlink_current_module = p_mod`, calls resolver, restores old value |
| **Runtime** | `_TRACKED` macros save old value, set `udynlink_current_module = p_mod`, call, restore old value |
| **Cross-module thunks** | Thunks do NOT update `udynlink_current_module`. The global reflects the **trust boundary** (the module the host invoked), not the immediate caller through a thunk. If mod_A calls mod_B via thunk, callbacks still see mod_A. |
| **Untracked calls** | Using `UDYNLINK_CALL` (without `_TRACKED`) does NOT set the global. Callbacks see whatever was set by the outermost tracked call or load-time resolution. |

### Zero-cost analysis

| When NOT using caller tracking | Cost |
|------|------|
| `udynlink_current_module` global | 4 bytes BSS (always present — same as any library global) |
| Load-time save/set/restore | ~12-16 bytes flash in 3 loader functions (always compiled) |
| `_TRACKED` macros | Zero — only compiled if used |
| **When NOT using `_TRACKED` macros and NOT reading the global** | 4 bytes BSS + ~16 bytes flash |

This is the same pattern as the weak defaults for `udynlink_external_vprintf` — they're always linked but cost nothing meaningful if unused. The 4 bytes of BSS adds at most one extra iteration to the startup .bss zeroing loop.

### Nesting behavior

The save/restore pattern handles nesting correctly:

```
Host → UDYNLINK_CALL_TRACKED(mod_A)
  udynlink_current_module = mod_A (saved NULL)
  mod_A runs, calls host_callback_X
    callback_X reads udynlink_current_module → mod_A ✓
    callback_X → UDYNLINK_CALL_TRACKED(mod_B)
      udynlink_current_module = mod_B (saved mod_A)
      mod_B runs, calls host_callback_Y
        callback_Y reads udynlink_current_module → mod_B ✓
      restores udynlink_current_module = mod_A
    callback_X continues, reads udynlink_current_module → mod_A ✓
  restores udynlink_current_module = NULL
```

Same pattern for load-time: if `udynlink_load_module_image()` is called while another module is already being loaded (e.g., during dependency resolution), the save/restore ensures the outer module is visible after the inner load completes.

## Implementation Details

### Load-time: Changes to `udynlink/udynlink.c`

The core loader sets `udynlink_current_module` around every call path that invokes the resolver:

1. **`udynlink_load_module_image()`** — around the `udynlink_load_apply_relocations()` call (line 512)
2. **`udynlink_link_incremental()`** — around `apply_extern_relocations_impl()` (line 744)
3. **`udynlink_relink_all()`** — around `apply_extern_relocations_impl()` (line 750)

Pattern in each:
```c
udynlink_module_t *_prev_mod = udynlink_current_module;
udynlink_current_module = p_mod;
// ... work that calls resolve_symbol() ...
udynlink_current_module = _prev_mod;
```

`udynlink.c` declares `extern udynlink_module_t *udynlink_current_module;` — no header include needed.

### Runtime: New `udynlink/udynlink_caller.h`

Tracked variants of the macros from `udynlink_call.h`:

```c
#define UDYNLINK_CALL_TRACKED(p_func, ret_type, args) \
    ({ \
        udynlink_module_t *_prev_mod = udynlink_current_module; \
        udynlink_current_module = (udynlink_module_t *)(p_func)->p_mod; \
        ret_type _result = UDYNLINK_CALL(p_func, ret_type, args); \
        udynlink_current_module = _prev_mod; \
        _result; \
    })
```

Same pattern for `_TRACKED_VOID` and `_TRACKED_MODULE_FUNC`. `UDYNLINK_PREPARE_CALL_TRACKED` just sets both `udynlink_current_module` and `r9` (no save/restore — matches the contract of `UDYNLINK_PREPARE_CALL`).

### New `src/udynlink_caller.c`

Single definition:
```c
udynlink_module_t *udynlink_current_module = NULL;
```

(Plus license header, includes.)

### CMakeLists.txt

Add `src/udynlink_caller.c` to the library sources. Add `udynlink_caller.h` to the install headers.

## Task Breakdown

### Task 1: Create `udynlink/udynlink_caller.h` + `src/udynlink_caller.c`
**Context**: See `udynlink/udynlink_call.h` for the existing macro patterns (especially `UDYNLINK_CALL` at line 113). The `_TRACKED` macros wrap those with save/set/restore of `udynlink_current_module`. The `.c` file is just the global definition.
**Agent**: `quick`
**Deliverables**: `udynlink/udynlink_caller.h`, `src/udynlink_caller.c`

### Task 2: Add load-time tracking to `udynlink/udynlink.c`
**Context**: Three call sites need the save/set/restore pattern:
- `udynlink_load_module_image()` around line 512 (around `udynlink_load_apply_relocations()`)
- `udynlink_link_incremental()` around line 744 (around `apply_extern_relocations_impl()`)
- `udynlink_relink_all()` around line 750 (around `apply_extern_relocations_impl()`)

Add `extern udynlink_module_t *udynlink_current_module;` near the top of the file (after includes). Each site: save, set to `p_mod`, do work, restore.

Also update the `udynlink_link_symbol()` function — but since it doesn't call `resolve_symbol()` (the host provides the address directly), no change is needed there. However, for consistency, `udynlink_link_symbol()` could also set the global so that if the host's calling code needs context during the patch, it's available. **Decision: don't set it in `udynlink_link_symbol` — the host already knows which module it's patching since it's providing the address.**

**Agent**: `quick`
**Deliverables**: Modified `udynlink/udynlink.c`

### Task 3: Wire into CMake build
**Context**: Add `src/udynlink_caller.c` to `add_library` sources in root `CMakeLists.txt`. Add `udynlink_caller.h` to the install headers list.
**Agent**: `quick`
**Deliverables**: Modified `CMakeLists.txt`

### Task 4: Test case
**Context**: Add a test that validates:
1. Runtime: module calls a host callback that reads `udynlink_current_module` — verifies it's the calling module
2. Runtime nesting: mod_A's callback calls mod_B, inner callback sees mod_B, after return outer callback sees mod_A
3. Load-time: resolver reads `udynlink_current_module` during `udynlink_load_module()` — verifies it's the loading module
4. After call returns: `udynlink_current_module` is properly restored (NULL or previous)

Look at `tests/test-globals1/` for how existing tests are structured (module .c file, host test .c file, test registration in `test_data.py`).

**Agent**: `agent`
**Deliverables**: New test under `tests/test-caller-tracking/`, updated `test_data.py`

### Task 5: Update documentation
**Context**: Update these files:
- `AGENTS.md` — add `udynlink_caller.h` row to the Public Headers table
- `docs/api-reference.md` — document `udynlink_current_module` and all `_TRACKED` macros
- `docs/examples.md` — add example of using caller tracking in a host callback
- `docs/integrating-as-host.md` — add "Module identity in callbacks" section
- `codemap.md` — add entries for the new files

**Agent**: `quick`
**Deliverables**: Updated documentation files

## Rejected Alternatives

| Option | Why rejected |
|--------|-------------|
| r9-based reverse lookup | O(n) scan, needs host's module registry, framework-y |
| Per-callback thunks | RAM-expensive (N×M thunks), complex |
| Modified resolver signature | Breaks existing API; the global achieves the same without signature changes |
| Thunk gateways update the global | Adds overhead to every cross-module call; trust-boundary semantics are simpler and more useful |
