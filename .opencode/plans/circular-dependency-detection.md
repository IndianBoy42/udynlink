# Plan: Circular Dependency Detection & Documentation

## Goal

Document the circular dependency failure mode and discourage users from creating them. Implement a **small, simple** loader-side detection for circular dependencies during load, with a specific error code.

## What Changed from the Previous Plan

The previous two-phase loading plan (`udynlink_load_module_deferred()` + `udynlink_link_module()`) was deemed a substantial refactor. This plan is a **minimal, additive** change:
- One new optional weak callback
- One new error code
- ~10 lines of new logic in existing load paths
- Small test host update
- One new integration test
- Documentation updates

## The Problem

If `mod_a` depends on `mod_b` and `mod_b` depends on `mod_a`:

1. Host loads `mod_a` → loader looks up `mod_b` via `udynlink_external_get_module_handle("mod_b")` → **NULL** → `UDYNLINK_ERR_LOAD_MISSING_DEP`.
2. Host loads `mod_b` → loader looks up `mod_a` → **NULL** → `UDYNLINK_ERR_LOAD_MISSING_DEP`.

Both fail with the same generic error. The user cannot tell it's a cycle vs a genuinely missing module.

## Simple Detection Approach

The loader is stateless and has no global module table. However, the **host** tracks which modules are currently loading (in between `udynlink_load_module()` entry and exit). By asking the host a single yes/no question, the loader can detect when a dependency is already in the middle of being loaded — a cycle.

### New Callback (Optional, Weak Default)

```c
// In udynlink/udynlink_externals.h
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
```

**Weak default** (returns 0 for all names) means existing hosts are not broken.

### Loader Logic

In `udynlink_load_module()` dependency loop (memory path, line ~325):

```c
udynlink_module_t *dep_mod = udynlink_external_get_module_handle(dep_str);
if (dep_mod == NULL) {
    if (udynlink_external_is_module_loading(dep_str)) {
        UDYNLINK_DEBUG(UDYNLINK_DEBUG_ERROR,
            "Circular dependency detected: '%s' depends on '%s' which is currently loading\n",
            udynlink_get_module_name(p_mod), dep_str);
        res = UDYNLINK_ERR_LOAD_CIRCULAR_DEP;
    } else {
        res = UDYNLINK_ERR_LOAD_MISSING_DEP;
    }
    goto exit;
}
```

Same logic in `udynlink_load_module_from_stream()` (streaming path, line ~672).

### Why This Works

- Host starts loading `mod_a` → registers `mod_a` as "loading" → calls `udynlink_load_module()`.
- Loader processes `mod_a` dependencies → sees `mod_b` → `get_module_handle("mod_b")` = NULL → `is_module_loading("mod_b")` = 0 → `MISSING_DEP`.
- Host then starts loading `mod_b` → registers `mod_b` as "loading" → calls `udynlink_load_module()`.
- Loader processes `mod_b` dependencies → sees `mod_a` → `get_module_handle("mod_a")` = NULL → `is_module_loading("mod_a")` = 1 → `CIRCULAR_DEP`.

The cycle is detected when the second module in the cycle is loaded.

### Bonus: Self-Dependency Check

Even without the callback, a module depending on itself is an obvious cycle. Add an inexpensive `strcmp` against the current module's own name:

```c
if (!strcmp(dep_str, udynlink_get_module_name(p_mod))) {
    res = UDYNLINK_ERR_LOAD_CIRCULAR_DEP;
    goto exit;
}
```

This catches the trivial A→A case with zero host support.

---

## Task Breakdown

### Task 1: Loader Changes (Small)

| Subtask | Description | Agent | File |
|---------|-------------|-------|------|
| 1.1 | Add `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` to error enum | `quick` | `udynlink/udynlink.h` |
| 1.2 | Add `udynlink_external_is_module_loading()` declaration to `udynlink_externals.h` | `quick` | `udynlink/udynlink_externals.h` |
| 1.3 | Add weak default implementation | `quick` | `udynlink/udynlink.c` (or new file) |
| 1.4 | Insert self-dependency check + `is_module_loading` check in memory-path dep loop | `agent` | `udynlink/udynlink.c` |
| 1.5 | Insert same checks in streaming-path dep loop | `agent` | `udynlink/udynlink.c` |

### Task 2: Test Host Update

| Subtask | Description | Agent | File |
|---------|-------------|-------|------|
| 2.1 | Add `g_loading_modules[]` array and `udynlink_external_is_module_loading()` implementation | `quick` | `tests/qemu_host/src/main.c` |
| 2.2 | Update `test_load_module()` to register as loading before calling `udynlink_load_module()`, and unregister on failure | `quick` | `tests/qemu_host/src/test_utils.c` |

### Task 3: Integration Test

| Subtask | Description | Agent | Deliverable |
|---------|-------------|-------|-------------|
| 3.1 | Create `tests/test-circular-deps/` with `mod_self_dep.c` (depends on itself) and `test_qemu.c` | `agent` | New test directory |
| 3.2 | Create `test_data.py` for the test driver | `quick` | `tests/test-circular-deps/test_data.py` |
| 3.3 | Optionally create a two-module cycle test (`mod_a.c` + `mod_b.c`) if harness supports loading two modules where second detects cycle | `agent` | Additional test cases |

### Task 4: Documentation

| Subtask | Description | Agent | File |
|---------|-------------|-------|------|
| 4.1 | Expand `docs/how-it-works.md` "Circular Dependency Detection" section with failure mode explanation and discouragement | `quick` | `docs/how-it-works.md` |
| 4.2 | Add note to `docs/writing-modules.md` `--depends` option description | `quick` | `docs/writing-modules.md` |
| 4.3 | Update `docs/integrating-as-host.md` with `udynlink_external_is_module_loading` callback documentation | `quick` | `docs/integrating-as-host.md` |
| 4.4 | Update `docs/api-reference.md` error code table | `quick` | `docs/api-reference.md` |
| 4.5 | Update `README.md` if it mentions dependencies | `quick` | `README.md` |
| 4.6 | Update `AGENTS.md` known issues list | `quick` | `AGENTS.md` |

### Task 5: CI Verification

| Subtask | Description | Agent | Deliverable |
|---------|-------------|-------|-------------|
| 5.1 | Run `just test-mps2` to verify no regressions | `agent` | All 25 tests pass |
| 5.2 | Run `just test-f429-single test-circular-deps` | `agent` | New test passes |

---

## Context Guide for Implementation Agents

### Key Files

| File | Why Read It | Key Lines |
|------|-------------|-----------|
| `udynlink/udynlink.h` | Error enum | Lines 156-172 (`UDYNLINK_ERROR_CODES`) |
| `udynlink/udynlink.c` | Load paths | Lines 252-473 (`udynlink_load_module`), 606-856 (`udynlink_load_module_from_stream`), dep loops at 305-336 and 659-683 |
| `udynlink/udynlink_externals.h` | Callback declarations | Lines 100-155 (existing callbacks) |
| `tests/qemu_host/src/main.c` | Test host registry | Lines 20-36 (`g_modules[]`), 77-85 (`udynlink_external_get_module_handle`) |
| `tests/qemu_host/src/test_utils.c` | Test wrappers | Lines 60-71 (`test_load_module`, `test_unload_module`) |
| `tests/test-deps/` | Existing dep test | Reference for test harness and `--depends` usage |
| `docs/how-it-works.md` | Existing circular dep mention | Lines 672-674 |

### Weak Default Pattern

The codebase already uses weak defaults for callbacks:

```c
// tests/qemu_host/src/main.c:52-62
uint32_t test_resolve_symbol(const char *name) __attribute__((weak));
uint32_t test_resolve_symbol(const char *name) {
    (void)name;
    return 0;
}
```

Follow the same pattern for `udynlink_external_is_module_loading`.

### Test Host Loading State

The test host tracks loaded modules in `g_modules[]` with `g_module_count`. To support `is_module_loading`, add a parallel array `g_loading_modules[]` and `g_loading_count`. The lifecycle:

1. `test_load_module()` called:
   a. Register `p_mod` in `g_loading_modules[]` (temporarily)
   b. Call `udynlink_load_module(p_mod, ...)`
   c. If success: `udynlink_test_register_module(p_mod)` (existing loaded registry) + remove from `g_loading_modules[]`
   d. If failure: remove from `g_loading_modules[]`

However, `p_mod` doesn't have a name until AFTER `udynlink_load_module()` populates `p_header`. So the loading registry needs to store the **base_addr** or **module name** (read from image via `udynlink_get_module_name_from_image(base_addr)`) instead of the handle.

```c
static const void *g_loading_addrs[UDYNLINK_MAX_MODULES];
static int g_loading_count = 0;

void udynlink_test_register_loading(const void *base_addr) {
    if (g_loading_count < UDYNLINK_MAX_MODULES)
        g_loading_addrs[g_loading_count++] = base_addr;
}

void udynlink_test_unregister_loading(const void *base_addr) {
    for (int i = 0; i < g_loading_count; i++) {
        if (g_loading_addrs[i] == base_addr) {
            g_loading_addrs[i] = g_loading_addrs[--g_loading_count];
            return;
        }
    }
}

int udynlink_external_is_module_loading(const char *module_name) {
    for (int i = 0; i < g_loading_count; i++) {
        const char *name = udynlink_get_module_name_from_image(g_loading_addrs[i]);
        if (name && !strcmp(name, module_name)) return 1;
    }
    return 0;
}
```

And update `test_utils.c`:

```c
udynlink_error_t test_load_module(udynlink_module_t *p_mod, const void *base_addr, ...) {
    udynlink_test_register_loading(base_addr);
    udynlink_error_t err = udynlink_load_module(p_mod, base_addr, ...);
    if (err != UDYNLINK_OK) {
        udynlink_test_unregister_loading(base_addr);
        return err;
    }
    udynlink_test_unregister_loading(base_addr);
    udynlink_test_register_module(p_mod);
    return UDYNLINK_OK;
}
```

### Pitfalls

1. **The self-dependency check must happen BEFORE the `is_module_loading` check**, because `is_module_loading` for the current module will return 0 (it's not registered as loading yet in the host's registry at the time the dep loop runs — or it IS, depending on when the host registers. Actually, if the host registers before calling `udynlink_load_module()`, then `is_module_loading(current_module_name)` would return 1. So self-dependency should be detected via direct `strcmp` against the current module's name, not via the callback.)

2. **Weak symbol placement**: Some linkers require weak definitions to be in the same translation unit as the strong definition, or at least visible. Putting the weak default in `udynlink.c` is safest (same file that calls it), or in a dedicated `udynlink_weak_defaults.c`. The test host will provide a strong override.

3. **Streaming path**: The streaming loader reads dependency names one at a time from the stream. The same `is_module_loading` check applies there.

4. **Don't forget to update `tests/test_utils.c`** — it's easy to miss since the load wrappers are there, not in `main.c`.

## Decisions (Confirmed)

| # | Decision | Rationale |
|---|----------|-----------|
| D1 | Use optional weak callback instead of global loader state | Keeps loader stateless; minimal host contract addition |
| D2 | Add self-dependency check in addition to callback check | Catches trivial cycles with zero host support |
| D3 | `is_module_loading` operates on module names (not handles) | The host's loading registry stores base_addr; names are read via `udynlink_get_module_name_from_image()` |
| D4 | Error code is `UDYNLINK_ERR_LOAD_CIRCULAR_DEP` | Reuses existing `LOAD_*` prefix pattern; placed after `UDYNLINK_ERR_LOAD_MISSING_DEP` in enum |
| D5 | Test host tracks loading state | Necessary for cycle detection to work in integration tests |
| D6 | Only one new test needed | `test-circular-deps` with self-dependency is sufficient; two-module cycle test is optional bonus |

