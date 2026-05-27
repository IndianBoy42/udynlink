# Plan: Per-Module LOT Base + Dependency System Removal

> **Status:** Architecture locked. Ready for execution.
> **Goals:**
> 1. Eliminate `UDYNLINK_LOT_BASE_ADDR` — host sets `r9` directly.
> 2. Remove the entire built-in dependency system — `--depends`, dep tracking, 3-tier resolution, dep-related error codes, dep-related tests, dep-related docs.
> 3. Keep low-level primitives: `UDYNLINK_SYM_DEFERRED`, `udynlink_link_symbol()`, `udynlink_link_incremental()`, `udynlink_relink_all()`, `udynlink_is_symbol_resolved()`.
> 4. Bump loader ABI to 3.0. Old modules must be rebuilt.

---

## 1. Architecture Overview

### 1.1 LOT Base: Host-Managed `r9` (Approach A)

The global fixed memory word is eliminated entirely.

- **Prologue template** simplified to only save/restore caller's `r9`:
  ```asm
  push    {r9, lr}
  bl      __actual_func
  pop     {r9, pc}
  ```
- **Host sets `r9`** before every call: `UDYNLINK_PREPARE_CALL(p_mod)` moves `ram_base` into `r9` via inline asm.
- **ISR safety:** The wrapper saves `r9` on the stack; NVIC exception entry doesn't need to save `r9` because the callee (the wrapper) preserves it.

### 1.2 Dependency System Removal

**Rationale:** The built-in dependency system was dangerously broken for module→module direct calls (LOT base never switches). Cross-module calls only worked for trivial leaf functions. Rather than fix this with complex RAM thunks, we remove it. Dependency tracking and cross-module linking can be built **on top of** the remaining low-level primitives (`link_symbol`, `link_incremental`, deferred symbols) by host firmware or a higher-level layer.

**Removed from the library:**
- `num_deps` and `deps_strtab_size` from module header
- Dependency string table from module binary format
- `num_deps`, `dep_refcount`, `max_deps`, `deps[]` from `udynlink_module_t`
- `--depends` flag from `mkmodule`
- `validate_dependencies()` from loader
- `resolve_symbol_tiered()` — dependency tier removed; symbol resolution becomes single tier via `udynlink_external_resolve_symbol()`
- `udynlink_external_get_module_handle()` callback
- `udynlink_external_is_module_loading()` callback
- `UDYNLINK_DEP_DEFERRED` macro
- Error codes: `UDYNLINK_ERR_LOAD_MISSING_DEP`, `UDYNLINK_ERR_LOAD_CIRCULAR_DEP`, `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS`
- Public APIs: `udynlink_image_get_deps()`, `udynlink_get_module_deps()`, `udynlink_is_module_fully_linked()`, `udynlink_get_linked_dependency()`
- All dependency-related tests

**Kept in the library:**
- `UDYNLINK_SYM_DEFERRED` — host callback can still defer individual extern symbols
- `udynlink_link_symbol()` — host manually patches a symbol's relocation slot
- `udynlink_link_incremental()` — re-resolves zeroed EXTERN slots
- `udynlink_relink_all()` — re-resolves all EXTERN slots from scratch
- `udynlink_is_symbol_resolved()` — query whether a symbol has a non-zero value
- `udynlink_external_resolve_symbol()` callback — sole resolution hook, single tier

### 1.3 Module Binary Format (ABI v3.0)

```
[Header 32 bytes]      ← back to original 32-byte size
[Relocations]          ← num_rels * 8 bytes
[Symbol Table]         ← symt_size bytes, rounded up to 4
[Code]                 ← code_size bytes, rounded up to 4
[Data]                 ← data_size bytes
```

- `udynlink_version` field in header becomes `3.0` (value `0x0300`).
- `num_deps` and `deps_strtab_size` fields removed.
- `mod_version` stays (module's own ABI version, independent of loader version).
- `arch_tag` stays (architecture compatibility check).

### 1.4 `--no-prologue` Mode

`--no-prologue` remains supported as an **advanced opt-in** for size-sensitive modules.

- **Behavior:** The module has no assembly wrapper. Exported functions are called directly by the host.
- **Host responsibility:** The caller must ensure `r9` is set to the module's `ram_base` before the call and restored afterwards.
- **Safe APIs:** `UDYNLINK_CALL` and `UDYNLINK_CALL_MODULE_FUNC` automatically save/restore `r9` around the call, so they work safely with `--no-prologue` modules.
- **Raw function pointers:** If the host casts `sym.val` to a function pointer and calls it directly, it must manually save/restore `r9` (or use `UDYNLINK_PREPARE_CALL` knowing that `r9` will be overwritten).
- **Documentation:** Clearly document in `docs/writing-modules.md` and `docs/integrating-as-host.md` that `--no-prologue` is for advanced users who manage `r9` explicitly.

### 1.5 Symbol Resolution (Single Tier)

With the dependency system removed, there is no longer any need for two resolution callbacks. The distinction between "critical" and "fallback" only existed to order resolution *before* and *after* dependency module search. Now there is only one resolution path:

```
1. udynlink_external_resolve_symbol(name)
   → if returned value > 0: use it
   → if UDYNLINK_SYM_DEFERRED: write 0, continue
   → if 0: fail (EXTERN) or keep module's own address (WEAK)
```

The host implements a single callback. It can implement any internal logic it wants (e.g., hash table lookup, then linear search, then dynamic resolution). The loader does not impose any tier ordering — that's entirely the host's responsibility.

- `udynlink_external_resolve_critical_symbol()` is removed entirely.
- `udynlink_external_resolve_symbol()` remains as the sole resolution hook.
- It can return `UDYNLINK_SYM_DEFERRED` to defer resolution to a later `link_incremental()` / `link_symbol()` call.

### 1.5 Weak Symbols

Weak symbols are still supported. At load time:
1. Write the module's own address into the relocation slot (default).
2. Call `udynlink_external_resolve_symbol()`. If an override is found, patch the slot.
3. If deferred, keep the module's own default.

Limitation unchanged: internal `bl` calls to weak functions still use the module's own definition because they are PC-relative and resolved at link time. Host override only works for data weak symbols, function pointers, and external callers via `udynlink_lookup_symbol()`.

---

## 2. Task Breakdown

### Phase 1: Core Runtime + Build Toolchain (Parallel)

#### Task 1: Core C/C++ Runtime Refactor
**Agent:** `agent`
**Files:** `udynlink/udynlink.h`, `udynlink/udynlink.c`, `udynlink/udynlink_call.h`, `udynlink/udynlink.hpp`, `udynlink/udynlink_externals.h`

**Changes in `udynlink.h`:**
1. Remove `num_deps` and `deps_strtab_size` from `udynlink_module_header_t`. Header returns to 32 bytes.
2. Update binary layout comments.
3. Remove `UDYNLINK_DEP_DEFERRED` macro.
4. Keep `UDYNLINK_SYM_DEFERRED` macro.
5. Remove `num_deps`, `dep_refcount`, `max_deps`, `deps` from `udynlink_module_t`.
6. Remove `UDYNLINK_ERR_LOAD_MISSING_DEP`, `UDYNLINK_ERR_LOAD_CIRCULAR_DEP`, `UDYNLINK_ERR_MODULE_HAS_DEPENDENTS` from error codes.
7. Remove `udynlink_image_get_deps()` declaration.
8. Remove `udynlink_get_module_deps()` declaration.
9. Remove `udynlink_is_module_fully_linked()` declaration.
10. Remove `udynlink_get_linked_dependency()` declaration.
11. Bump `UDYNLINK_LOADER_ABI_VERSION` to `UDYNLINK_MAKE_VERSION(3, 0)`.
12. Remove `UDYNLINK_LOT_BASE_ADDR` macro and `#ifndef` block.
13. Redefine `UDYNLINK_PREPARE_CALL()` to only set `r9`:
    ```c
    #define UDYNLINK_PREPARE_CALL(p_mod) do { \
        __asm volatile ("mov r9, %0" :: "r"((uint32_t)(p_mod)->ram_base) : "r9"); \
    } while(0)
    ```
14. Update doc comments for `udynlink_load_module()` (remove "resolves dependencies").
15. Update doc comments for `UDYNLINK_PREPARE_CALL`.

**Changes in `udynlink.c`:**
1. Remove `udynlink_external_is_module_loading()` weak stub.
2. Remove `udynlink_external_get_module_handle()` weak stub.
3. Remove `get_deps_strtab_offset()`, `get_deps_strtab()` static helpers.
4. Rewrite `get_code_offset_from_header()` — no longer adds `deps_strtab_size` or padding.
5. Rewrite `resolve_symbol_tiered()` → `resolve_symbol()` — removes the dependency module search loop. Single tier: host callback only.
6. Remove `validate_dependencies()` entirely.
7. In `udynlink_load_apply_relocations()`: update calls from `resolve_symbol_tiered()` to `resolve_symbol()`.
8. In `udynlink_load_module_image()`: remove `validate_dependencies()` call. Remove COPY_ALL path's dep strtab copy.
9. Rewrite `udynlink_cpp_init()` to use `UDYNLINK_PREPARE_CALL(p_mod)`.
10. Rewrite `udynlink_unload_module()` — remove `dep_refcount` check and dependency refcount decrement loop.
11. Remove `udynlink_image_get_deps()` implementation.
12. Remove `udynlink_get_module_deps()` implementation.
13. Update `udynlink_lookup_symbol()` — remove WEAK symbol's `resolve_symbol_tiered` call (should use `resolve_symbol()` without dep tier).
14. Remove `apply_extern_relocations_impl()`, `udynlink_link_incremental()`, `udynlink_relink_all()`, `udynlink_link_symbol()`, `udynlink_is_module_fully_linked()`, `udynlink_get_linked_dependency()` implementations.
    Wait — the user said **keep** `link_incremental`, `relink_all`, `link_symbol`, `is_symbol_resolved`. I must keep these!
15. Keep `udynlink_link_incremental()`, `udynlink_relink_all()`, `udynlink_link_symbol()`, `udynlink_is_symbol_resolved()` but update their internals to use `resolve_symbol()` (no dep tier).
16. Keep `apply_extern_relocations_impl()` but remove dep search from `resolve_symbol()`.
17. Update `udynlink_image_from_memory()` — remove `p_deps_strtab` assignment.
18. Ensure `udynlink_validate_header()` naturally rejects old modules (already checks `udynlink_version > loader_version`).

**Changes in `udynlink_externals.h`:**
1. Remove `udynlink_external_get_module_handle()` declaration.
2. Remove `udynlink_external_is_module_loading()` declaration.
3. Remove `udynlink_external_resolve_critical_symbol()` declaration entirely.
4. Update comments for `udynlink_external_resolve_symbol()` — remove "three-tier" language, describe single-tier.

**Changes in `udynlink_call.h`:**
1. Remove `UDYNLINK_SET_LOT_BASE` macro.
2. Update `UDYNLINK_SET_R9` doc comment.
3. Update `UDYNLINK_CALL` to **save/restore `r9` around the call**:
   ```c
   #define UDYNLINK_CALL(p_func, ret_type, args) \
       ({ \
           uint32_t _udynlink_prev_r9; \
           __asm volatile ("mov %0, r9" : "=r"(_udynlink_prev_r9) : :); \
           UDYNLINK_PREPARE_CALL((p_func)->p_mod); \
           ret_type _udynlink_result = ((ret_type (*)(...))(p_func)->addr) args; \
           __asm volatile ("mov r9, %0" :: "r"(_udynlink_prev_r9) : "r9"); \
           _udynlink_result; \
       })
   ```
4. Add `UDYNLINK_CALL_VOID(p_func, args)` for void-returning functions (same save/restore logic, no result variable).
5. Update `UDYNLINK_CALL_MODULE_FUNC` to use the save/restore-aware `UDYNLINK_CALL`.
6. Document that `UDYNLINK_CALL` is now safe for both prologued and `--no-prologue` modules because it always restores `r9`.
7. Update doc comments.

**Changes in `udynlink.hpp`:**
1. Rewrite `Context` class: `prev_lot_base_` → `prev_r9_` (type `uint32_t`).
2. Constructor reads `r9` into `prev_r9_`, then writes `ram_base` to `r9`.
3. Destructor restores `r9` from `prev_r9_`.
4. `rebind()` writes `ram_base` to `r9` directly.
5. Update doc comments (remove "NOT interrupt-safe" if appropriate, or keep the warning about raw pointer calls).

**Review criteria:**
- `grep -r "UDYNLINK_LOT_BASE_ADDR" udynlink/` returns zero matches.
- `grep -r "num_deps\|dep_refcount\|max_deps\|deps\[" udynlink/` returns zero matches (except in comments if we missed any).
- `grep -r "validate_dependencies\|resolve_symbol_tiered\|get_deps_strtab" udynlink/` returns zero matches.
- `grep -r "UDYNLINK_DEP_DEFERRED\|UDYNLINK_ERR_LOAD_MISSING_DEP\|UDYNLINK_ERR_LOAD_CIRCULAR_DEP\|UDYNLINK_ERR_MODULE_HAS_DEPENDENTS" udynlink/` returns zero matches.
- `grep -r "udynlink_external_resolve_critical_symbol" udynlink/` returns zero matches.
- Core library compiles: `cmake -B build -S . && cmake --build build`.

---

#### Task 2: Build Toolchain Refactor
**Agent:** `agent`
**Files:** `scripts/mkmodule`, `scripts/asm_template_*.tmpl`

**Changes in `scripts/mkmodule`:**
1. Remove `--depends` CLI argument and its parsing.
2. Remove `--lot-base` CLI argument and its parsing.
3. Remove `dep_list` variable.
4. Remove `deps_strtab` and `deps_strtab_size` from header generation.
5. Change default `--udynlink-version` from `2.0` to `3.0`.
6. Remove auto-bump logic that set `udynlink_version = 2.0` when `--depends` was used.
7. Remove `lot_base` from Jinja2 render dict.
8. Update `udynlink_image_from_memory()` logic (in the Python script's module generation) — no `p_deps_strtab`.
9. Simplify binary layout: header (32 bytes) → relocs → symtab → code → data. No dep strtab, no padding for dep strtab.
10. Update help text.

**Changes in `scripts/asm_template_armv6m.tmpl`:**
1. Remove literal pool `.word {{lot_base}}`.
2. Remove `ldr r1, .L1{{s}}`, `ldr r2, [r1]`, `mov r9, r2` sequence.
3. Remove `push {r1}` / `pop {r1}`.
4. Simplify to save caller's `r9` via `r2`, call actual function, restore `r9`.

**Changes in `scripts/asm_template_armv7m.tmpl`:**
1. Remove literal pool and `ldr` sequence.
2. Simplify to `push {r9, lr}` / `bl` / `pop {r9, pc}`.

**Changes in `scripts/asm_template_armv8m.tmpl`:**
1. Same simplification as v7m.

**Changes in `scripts/asm_template.tmpl` (old fallback):**
1. Same simplification as v7m, or delete if no longer referenced.

**Changes in `scripts/targets.py`:**
1. Verify no `lot_base` or dependency references (should be clean).

**Review criteria:**
- `mkmodule --help` shows no `--depends` or `--lot-base`.
- `just module hello.c` produces a module with `udynlink_version == 0x0300` in the header.
- `arm-none-eabi-objdump -d` on generated prologue shows no `.word` literal pool.
- Module binary size is smaller (no dep strtab, shorter prologue).

---

### Phase 2: Test Suite Migration

#### Task 3: Test Suite Refactor
**Agent:** `agent`
**Files:** All `tests/` directories

**Remove entirely:**
- `tests/test-deps/` — tests the removed dependency loading system
- `tests/test-circular-deps/` — tests circular dependency detection
- `tests/test-circular-deps-link/` — tests deferred dependency linking
- `tests/test-optional-dep/` — tests optional dependencies

**Update:**
- `tests/test-deferred-symbol/test_qemu.c` — remove any `UDYNLINK_LOT_BASE_ADDR` writes, replace with `UDYNLINK_PREPARE_CALL()`. Keep the deferred symbol test logic (it tests `UDYNLINK_SYM_DEFERRED` + `link_symbol`, which we keep).
- `tests/test-link-symbol/test_qemu.c` — same LOT base migration. Keep link_symbol test logic.
- `tests/test-weak-symbols/test_qemu.c` — same LOT base migration.
- `tests/test-streaming-load/test_qemu.c` — same LOT base migration.
- `tests/test-globals1/test_qemu.c` — same LOT base migration.
- `tests/test-globals2/test_qemu.c` — same LOT base migration.
- Any other `test_qemu.c` files referencing `UDYNLINK_LOT_BASE_ADDR`.

**Update test host:**
- `tests/qemu_host/src/main.c`:
  - Remove `udynlink_external_get_module_handle()` implementation.
  - Remove `udynlink_external_is_module_loading()` implementation.
  - Remove `g_loading_names[]`, `g_loading_addrs[]`, `g_loading_count` state.
  - Remove `udynlink_external_resolve_critical_symbol()` implementation.
  - Update `udynlink_external_resolve_symbol()` comments.
  - Remove any dep-related logic from host callbacks.

**Update platform CMake files:**
- All `tests/platforms/*/platform.cmake` — remove `UDYNLINK_LOT_BASE_ADDR=0x20000000` from `UDYNLINK_PLATFORM_DEFINES`.

**Update linker scripts:**
- `tests/platforms/olimex_stm32_h405/mem.ld` — remove comment about `UDYNLINK_LOT_BASE_ADDR`.

**Update test descriptors:**
- Any `test_data.py` referencing removed tests or dep-related logic.

**Review criteria:**
- `grep -r "UDYNLINK_LOT_BASE_ADDR" tests/` returns zero matches.
- `grep -r "udynlink_external_get_module_handle\|udynlink_external_is_module_loading" tests/` returns zero matches.
- `ls tests/test-*` shows no `test-deps`, `test-circular-deps`, `test-circular-deps-link`, `test-optional-dep`.
- `just test-mps2` builds and runs successfully.

---

### Phase 3: Validation

#### Task 4: Full Validation
**Agent:** `shell` + `agent`

1. Build core library: `cmake -B build -S . && cmake --build build`
2. Run `just validate-all-targets` to ensure all 9 targets compile `hello.c`.
3. Run `just test-mps2` (MPS2-AN386, Cortex-M4).
4. Run `just test-an385` (MPS2-AN385, Cortex-M3).
5. Run `just test-an500` (MPS2-AN500, Cortex-M7).
6. Run `just test-an505` (MPS2-AN505, Cortex-M33).
7. Run `just test-h405` (Olimex H405, Cortex-M4F).
8. Run `just test-f429` (STM32F429, legacy QEMU).
9. If any test fails, debug and fix (may require revisiting Tasks 1–3).

**Review criteria:**
- `just ci` passes with no failures.
- Test count should be reduced by the number of removed dep tests (~4 test suites), but all remaining tests pass.
- No regressions in existing non-dep tests.

---

### Phase 4: Documentation (Parallel with Phase 1–3 for drafting, finalize after Phase 4)

#### Task 5: Documentation Rewrite
**Agent:** `agent`
**Files:** `docs/`, `README.md`, `AGENTS.md`, `CHANGELOG.md`, `codemap.md`

**Rewrite `docs/how-it-works.md`:**
- Remove "Module Dependencies (ABI 2.0+)" section.
- Rewrite "The LOT Base Address Convention" — explain host-managed `r9`.
- Update assembly listings for all three prologue variants.
- Update module binary format diagram (no dep strtab).
- Update "Symbol Resolution at Load Time" — single-tier (host callback only).
- Update "Relocation Processing" — no dependency tier.

**Rewrite `docs/integrating-as-host.md`:**
- Remove all dependency callback documentation (`get_module_handle`, `is_module_loading`).
- Rewrite "Set up the LOT base" to use `UDYNLINK_PREPARE_CALL()`.
- Rewrite "Symbol Resolution" — single tier (host callback only).
- Remove "Deferred Dependencies" section.
- Remove "Circular Dependencies" section.
- Update troubleshooting: remove "dependency not found", "circular dependency".
- Update thread safety notes (remove `dep_refcount` references).

**Rewrite `docs/writing-modules.md`:**
- Remove `--depends` flag documentation.
- Remove "optional dep detection".
- Update `mkmodule` flag reference.
- Remove "Dependency Modules" section.

**Rewrite `docs/api-reference.md`:**
- Remove all removed APIs (`image_get_deps`, `get_module_deps`, `is_module_fully_linked`, `get_linked_dependency`).
- Remove `UDYNLINK_LOT_BASE_ADDR`.
- Update `UDYNLINK_PREPARE_CALL` docs.
- Update `udynlink_cpp_init` docs.
- Update error code table.
- Update `udynlink_module_header_t` and `udynlink_module_t` struct docs.

**Rewrite `docs/examples.md`:**
- Remove all dependency examples.
- Update every example that sets LOT base to use `UDYNLINK_PREPARE_CALL()`.
- Keep `link_symbol`, `link_incremental`, deferred symbol examples.

**Rewrite `docs/testing.md`:**
- Remove dependency test examples.
- Update `test_data.py` documentation (no `--depends`).

**Update `README.md`:**
- Update quickstart example.
- Remove `--depends` from feature list.
- Update "How it works" summary.

**Update `AGENTS.md`:**
- Remove all dependency references (`--depends`, three-tier, `dep_refcount`, circular deps).
- Update architecture notes.
- Update build command descriptions.
- Update the "Fixed LOT Base" codemap note.

**Update `CHANGELOG.md`:**
- Add entry for v3.0: "Removed built-in dependency system and UDYNLINK_LOT_BASE_ADDR. Host now manages r9 directly."

**Update `codemap.md`:**
- Remove dependency test listings.
- Update header struct description.

**Review criteria:**
- `grep -r "UDYNLINK_LOT_BASE_ADDR" docs/ README.md AGENTS.md CHANGELOG.md codemap.md` returns zero matches.
- `grep -r "depends\|dependency\|three-tier\|dep_refcount\|circular dep" docs/ README.md AGENTS.md` returns zero matches (except historical changelog).
- `grep -r "UDYNLINK_DEP_DEFERRED\|get_module_handle\|is_module_loading" docs/ README.md AGENTS.md` returns zero matches.

---

## 3. Execution Order & Parallelization

```
[Task 1: Core Runtime] ───┐
                          ├──→ [Task 3: Test Suite] ──→ [Task 4: Validation]
[Task 2: Build Toolchain] ─┘

[Task 5: Docs] ───────────────→ (draft in parallel, finalize after Task 4)
```

- **Phase 1:** Run Task 1 and Task 2 in parallel.
- **Phase 2:** Run Task 3 after Phase 1 completes.
- **Phase 3:** Run Task 4 after Phase 2 completes.
- **Phase 4:** Run Task 5 in parallel with Phases 1–3 for drafting, then finalize after Task 4.

---

## 4. Risk Register

| Risk | Mitigation |
|------|----------|
| M0 prologue inline asm push/pop restrictions | Verify generated prologue disassembly with `arm-none-eabi-objdump` before running QEMU tests. |
| C++ `Context` inline asm syntax portability | Test with CI compiler. Use `"r"` constraints only. |
| Old test host `main.c` has hidden dep references | Audit `tests/qemu_host/src/main.c` carefully; grep for `dep`, `module_handle`, `loading`. |
| `udynlink_validate_header()` rejects v3.0 modules on old code (not a risk for us, but for downstream) | Already accepted: ABI break is intentional. Document clearly in CHANGELOG. |
| `get_code_offset_from_header()` change breaks existing non-dep modules | Only affects the padding calculation. Existing modules without deps already had `deps_strtab_size=0`, so `code_offset` is identical. No binary compatibility issue for old non-dep modules, but they still get rejected by version check. |
| Weak symbol resolution without dep tier may break `test-weak-symbols` | Update the test if it expected dep-tier override. The test should now only test host override. |
| `link_incremental` / `relink_all` still call `resolve_symbol()` which no longer has dep tier | This is correct: these APIs now only resolve via host callbacks, which is the intended behavior. |

---

## 5. Decision Checkpoints

All major decisions are locked:

1. ✅ **Approach A confirmed** — Eliminate `UDYNLINK_LOT_BASE_ADDR`, host sets `r9` directly.
2. ✅ **Dependency system removal confirmed** — Remove `--depends`, dep tracking, 3-tier resolution.
3. ✅ **Low-level primitives kept** — `UDYNLINK_SYM_DEFERRED`, `link_symbol`, `link_incremental`, `relink_all`, `is_symbol_resolved`.
4. ✅ **ABI break accepted** — Loader ABI 3.0. Old modules rejected.
5. ✅ **Prologue remains default** — Wrapper still saves/restores `r9` for safety. `--no-prologue` stays as opt-in.

**Ready to execute. Shall I begin delegating Tasks 1 and 2 in parallel?**

---

# Part 2: Standalone Dependency System (`udynlink_deps`)

> **Status:** Architecture draft. Pending review.
> **Goal:** Rebuild cross-module linking as an **optional standalone layer** on top of the v3.0 core, using only the existing API hooks. No changes to `udynlink.h` / `udynlink.c`.

---

## 2.1 Design Principles

1. **Zero core changes** — The dependency system is a separate header+source pair. Hosts that don't use it pay zero code/RAM cost.
2. **Dependency declarations via dummy symbols** — Modules import `.udynlink.mod.requires.{name}` as extern symbols. The host's existing `udynlink_external_resolve_symbol()` callback serves as the "please load this dependency" hook.
3. **Cross-module function calls via runtime-generated thunks** — Each cross-module function pointer gets a small RAM thunk that saves the caller's `r9`, sets it to the callee's `ram_base`, calls the real function, and restores `r9`.
4. **Single-tier symbol resolution** — The host callback handles everything: regular symbols, dependency declarations, and cross-module symbol lookups. The standalone header provides helpers the host calls from its callback.

---

## 2.2 Dummy Symbol Encoding

### Module Writer API

```c
#include "udynlink/udynlink_deps.h"

// In module source:
UDYNLINK_REQUIRES(math_lib);
UDYNLINK_REQUIRES(io_lib);
```

### Macro Expansion

```c
#define UDYNLINK_REQUIRES(mod_name) \
    extern udynlink_module_t *__udynlink_dep_##mod_name \
    __asm__(".udynlink.mod.requires." #mod_name)
```

This creates an **EXTERN** symbol named `.udynlink.mod.requires.math_lib` in the module's symbol table. At load time, the core loader treats it like any other unresolved symbol and calls `udynlink_external_resolve_symbol()`.

### Why EXTERN (not WEAK)

Using a strong extern reference means:
- The loader **fails the module load** if the host doesn't resolve the dependency.
- The host callback **must** handle `.udynlink.mod.requires.*` names, or the module can't load.
- This is correct behavior: a module that declares dependencies needs a dependency-aware host.

Using WEAK would be problematic because the v3.0 loader applies `offset_sym()` to WEAK symbols, adding the module's data/code base to the symbol value. For an undefined weak, the value is 0, so the slot would get the base address — which is wrong for a dependency handle.

---

## 2.3 Host Integration

### The Host's `udynlink_external_resolve_symbol()`

The host implements one callback that delegates to the standalone header:

```c
#include "udynlink/udynlink_deps.h"

static udynlink_thunk_pool_t g_thunk_pool;
static udynlink_dep_mgr_t g_dep_mgr;

uintptr_t udynlink_external_resolve_symbol(const char *name) {
    // 1. Check if it's a dependency declaration
    if (udynlink_dep_is_dependency(name)) {
        return udynlink_dep_resolve_dependency(&g_dep_mgr, name);
    }

    // 2. Check if it's a cross-module function call
    //    (the header looks up 'name' in loaded dependency modules)
    uintptr_t thunk = udynlink_dep_resolve_func(&g_dep_mgr, &g_thunk_pool, name);
    if (thunk != 0) return thunk;

    // 3. Check if it's a cross-module data symbol
    uintptr_t data = udynlink_dep_resolve_data(&g_dep_mgr, name);
    if (data != 0) return data;

    // 4. Regular host symbol table lookup
    return my_host_symbol_lookup(name);
}
```

### Dependency Resolution Flow

1. Module A is loaded via `udynlink_dep_load()`.
2. Core loader encounters `.udynlink.mod.requires.B`.
3. Host callback calls `udynlink_dep_resolve_dependency(&g_dep_mgr, ".udynlink.mod.requires.B")`.
4. The dependency manager:
   - Checks if B is already loaded.
   - Checks if B is currently loading (circular dependency detection).
   - If not loaded, calls `udynlink_load_module()` (or `udynlink_dep_load()`) recursively for B.
   - Returns B's module handle address `(uintptr_t)dep_mod`.
5. The LOT slot for `.udynlink.mod.requires.B` gets the handle address.
6. The dependency manager records that A depends on B.

### Cross-Module Function Resolution

When module A calls `math_add(a, b)` which is defined in module B:
1. Core loader encounters `math_add` as an EXTERN symbol in A.
2. Host callback calls `udynlink_dep_resolve_func(&g_dep_mgr, &g_thunk_pool, "math_add")`.
3. The dependency manager searches all loaded modules for `math_add`.
4. If found in module B:
   - Allocate a thunk from the thunk pool.
   - The thunk saves caller's `r9`, sets `r9 = B->ram_base`, calls `math_add`, restores `r9`.
   - Return the thunk address.
5. The LOT slot for `math_add` gets the thunk address.
6. When A calls `math_add`, the thunk switches `r9` to B's base, so B's PIC code works correctly.

---

## 2.4 Thunk Design

### Two-Level Dispatch: Shared Module Gateway + Per-Function Stubs

Instead of each thunk redundantly storing the callee's `ram_base`, we use a **two-level dispatch** that shares the `ram_base` word across all stubs targeting the same module.

**Why two-level is a clear win:** A corrected size analysis shows the break-even is at N≈1, making it better for virtually all realistic cases (see table below).

| Approach | v7m/v8m | v6m | Notes |
|----------|---------|-----|-------|
| Inline thunk (original) | 22 bytes | 26 bytes | `ram_base` repeated per function |
| Two-level (stub + gateway) | 8N + 16 | 8N + 18 | `ram_base` stored once in gateway |
| **Break-even N** | **N ≈ 1.1** | **N ≈ 1.1** | Two-level wins for all N ≥ 2 |

### Per-Function Stub (all architectures)

```asm
stub_math_add:                     ; 8 bytes, one per cross-module function reference
    ldr     r0, [pc, #0]           ; load func_addr into r0 (loads from .word below)
    b       gateway_B              ; branch to module B's shared gateway
    .word   math_add               ; patched: actual function address
```

The `b` instruction does **not** modify `lr`. The caller's return address in `lr` remains intact. The gateway pushes `lr`, calls the target via `blx`, then pops `lr` back into `pc`.

**Stub size: 8 bytes** (2 instructions + 1 word). The `b` offset is patched at allocation time based on the distance to the module's gateway.

### Per-Module Gateway (v7m / v8m)

The gateway is generated at load time by copying a **flash-resident template** into the thunk pool and patching the `ram_base` word.

**Template (16 bytes, stored in flash as `const uint8_t[]`):**
```asm
gateway_B:
    push    {r9, lr}               ; 32-bit (r9 is high register)
    ldr     r9, [pc, #4]           ; 16-bit, loads from .word below
    blx     r0                     ; 16-bit, call func_addr (r0 from stub)
    pop     {r9, pc}               ; 32-bit
    .word   B->ram_base            ; 4 bytes, patched at load time
```

**Gateway size: 16 bytes.**

### Per-Module Gateway (v6m / M0)

M0 lacks `push {r9, lr}` (arbitrary register sets) and `blx` with high registers. The template uses `r4` as scratch:

**Template (18 bytes, stored in flash as `const uint8_t[]`):**
```asm
gateway_B:
    push    {r4, lr}               ; 16-bit
    mov     r4, r9                 ; 16-bit, save caller's r9 in r4
    ldr     r2, [pc, #8]           ; 16-bit, load ram_base
    mov     r9, r2                 ; 16-bit, set callee's r9
    blx     r0                     ; 16-bit, call func_addr (r0 from stub)
    mov     r9, r4                 ; 16-bit, restore caller's r9
    pop     {r4, pc}               ; 16-bit
    .word   B->ram_base            ; 4 bytes, patched at load time
```

**Gateway size: 18 bytes.**

### Template-Based Generation (No Hand-Written Assembly Files)

The gateway and stub templates are stored as **`const uint8_t` arrays in C code** (not separate `.S` files). At load time, the dependency manager:

1. Allocates RAM from the thunk pool.
2. `memcpy` the appropriate template from flash into the allocated slot.
3. Patches the `.word` value(s) and the `b` offset.

This approach:
- **Avoids separate assembly files** and toolchain complexity.
- **Keeps the template bytes verifiable** — they can be tested by disassembling the `const uint8_t` array with `arm-none-eabi-objdump`.
- **Supports all three architectures** with three small template arrays (~16-18 bytes each).

### Thunk Pool Layout

The thunk pool allocates per-module blocks contiguously:

```
[stub1]          // 8 bytes
[stub2]          // 8 bytes
...
[stubN]          // 8 bytes
[gateway_B]      // 16 bytes (v7m) or 18 bytes (v6m)
[stub1]          // 8 bytes (next module)
...
[gateway_C]      // 16/18 bytes
```

All stubs for module B branch to `gateway_B` via `b`. The distance is `8*(N-1)` bytes from the first stub, well within the `b` range (±2 KB Thumb-1, ±16 MB Thumb-2).

### Thunk Pool

```c
typedef struct {
    uint8_t *base;
    size_t size;
    size_t used;
} udynlink_thunk_pool_t;
```

The host provides a RAM buffer for the thunk pool. Typical size: 1-2 KB, enough for ~50-100 cross-module function references.

---

## 2.5 Data Symbol Handling (No Thunks Needed)

Cross-module **data symbols** (variables, constants) do **not** require thunks. The LOT already stores the absolute address of the variable. Access works as follows:

1. Module A's code loads the variable's address from its LOT via `r9`-relative addressing:
   ```asm
   ldr     r3, [pc, #lot_index]   // load LOT index
   ldr     r3, [r9, r3]           // load variable address from A's LOT
   ldr     r3, [r3, #0]           // dereference the variable
   ```
2. The LOT slot contains the **absolute address** of the variable in module B's data section.
3. Module A's `r9` points to A's LOT (correct). The variable address in the LOT is absolute, so no `r9` switching is needed.

**Resolution path for data symbols:**
1. Core loader encounters `extern int math_result;` in module A.
2. Host callback calls `udynlink_dep_resolve_data(&g_mgr, "math_result")`.
3. Dependency manager searches all loaded modules for `math_result`.
4. If found in module B, returns `sym.val` (the already-relocated absolute address of the variable).
5. The LOT/data slot for `math_result` gets this address directly.

**Function pointers stored in data:** If a data variable in module A holds a function pointer to module B, the value assigned at load time (via relocation) gets a **thunk address** (not the raw function address). The host's resolution logic naturally handles this because it calls `udynlink_dep_resolve_func()` for any EXTERN symbol, whether accessed via LOT (direct call) or data (function pointer).

**Weak data symbols across modules:** If module B defines `weak int global_counter` and module A imports it, the v3.0 loader first applies B's address as default, then calls the host callback. The host callback (via `udynlink_dep_resolve_data`) can override with another module's definition. The slot gets the correct absolute address.

## 2.6 Dependency Manager State

```c
typedef struct {
    // Registry of loaded modules
    udynlink_module_t **modules;
    size_t count;
    size_t capacity;

    // Circular dependency detection stack
    const char *loading_stack[8];
    size_t loading_depth;

    // Optional: dependency graph for refcount-based unload
    // (can be kept simple: just track direct deps per module)
} udynlink_dep_mgr_t;
```

The manager is host-allocated and host-managed. The standalone header provides insert/remove/lookup helpers.

---

## 2.7 Public API

### For Module Writers

| Macro | Purpose |
|-------|---------|
| `UDYNLINK_REQUIRES(name)` | Declare a dependency on module `name` |

### For Host Firmware

| Function | Purpose |
|----------|---------|
| `udynlink_dep_is_dependency(name)` | Check if a symbol name is a dep declaration |
| `udynlink_dep_get_name(name)` | Extract module name from dep symbol |
| `udynlink_thunk_pool_init(pool, buf, size)` | Initialize a thunk pool |
| `udynlink_thunk_alloc_stub(pool, func)` | Allocate a per-function stub (8 bytes) |
| `udynlink_thunk_link_gateway(pool, mod)` | Allocate/return a per-module gateway for `mod` |
| `udynlink_dep_mgr_init(mgr, buf, cap)` | Initialize dependency manager |
| `udynlink_dep_load(mgr, mod, base, addr, size, mode, pool)` | Load module with auto-deps and thunks |
| `udynlink_dep_unload(mgr, mod)` | Unload module, tracking refcounts |
| `udynlink_dep_resolve_dependency(mgr, name)` | Resolve a `.udynlink.mod.requires.*` symbol |
| `udynlink_dep_resolve_func(mgr, pool, name)` | Resolve a function from a loaded dependency |
| `udynlink_dep_resolve_data(mgr, name)` | Resolve a data symbol from a loaded dependency |
| `udynlink_dep_find(mgr, name)` | Find a loaded module by name |

---

## 2.8 mkmodule Changes (Minimal)

The standalone system doesn't require mkmodule changes for the dependency declarations themselves — the `UDYNLINK_REQUIRES` macro uses GCC's `__asm__("name")` attribute, which the compiler and linker handle natively.

However, we may want to add an optional `--gen-deps-header` flag to mkmodule that scans the module's symbol table and generates a C header with `#define UDYNLINK_REQUIRES(...)` lines, making it easier for module writers. This is a nice-to-have, not required for correctness.

---

## 2.8 Task Breakdown

| Task | Work | Agent | Parallel? |
|------|------|-------|-----------|
| **A** | Design review & header skeleton (`udynlink_deps.h`) | `deep` | — |
| **B** | Core implementation (`udynlink_deps.c`): thunk templates, dep manager, resolution helpers | `agent` | After A |
| **C** | Test suite: new tests for dependency loading, cross-module calls with data access, circular dep detection, deferred dep loading | `agent` | After B |
| **D** | mkmodule optional `--gen-deps-header` flag | `quick` | After A |
| **E** | Documentation: new `docs/dependencies.md` guide, update `docs/integrating-as-host.md` with dependency callback examples | `agent` | After B |
| **F** | Validation: `just ci` across all platforms | `shell` | After C |

---

## 2.9 Key Design Decisions

1. **Dummy symbols vs. header fields** — Dummy symbols leverage the existing symbol table and relocation machinery. No header format changes, no mkmodule changes for basic usage.
2. **Thunks vs. direct calls** — Thunks are the only way to make cross-module calls safe on all load modes (COPY_ALL, COPY_TEXT_DATA, XIP). Direct `bl` to another module's function leaves `r9` pointing to the caller's LOT.
3. **Two-level dispatch (gateway + stub)** — Per-module gateway stores `ram_base` once (16 bytes v7m, 18 bytes v6m). Per-function stubs are 8 bytes each. Saves ~14 bytes per cross-module function reference compared to inline thunks (22 bytes v7m, 26 bytes v6m). Break-even at N≈1.
4. **Trampoline in C, not assembly** — The gateway is a `__attribute__((naked))` C function with inline assembly. Easier to maintain than a separate `.S` file, but requires careful section placement to keep `b` range valid.
4. **Single callback vs. multiple callbacks** — The host still implements only `udynlink_external_resolve_symbol()`. The standalone header provides helpers the host calls from inside that one callback.
5. **No automatic module unloading** — `udynlink_dep_unload()` decrements refcounts but doesn't cascade. The host decides when to actually call `udynlink_unload_module()`. This avoids surprise unload side effects.

---

## 2.10 Open Questions

1. **M0 stub `b` range to gateway** — The `b` instruction in Thumb-1 has ±2 KB range. All gateways are placed in a contiguous `udynlink_gateways` section. The stub pool is allocated nearby. On typical Cortex-M systems with small RAM, this is easily satisfied. Document the constraint.
2. **Thunks and XIP** — Thunks live in RAM. For XIP modules, the module's code is in flash, but cross-module calls go through RAM thunks. This is fine.
3. **Thunk pool exhaustion** — `udynlink_thunk_alloc()` returns 0, causing the load to fail with `UDYNLINK_ERR_LOAD_OUT_OF_MEMORY`. Host sizes the pool appropriately.
4. **C++ module constructors calling deps** — If module A's `__init_array` calls a function from module B, B must be loaded before A's `__init_array` runs. `udynlink_dep_load()` resolves all dependencies before returning, so this is naturally handled.
5. **Data vs. function symbol distinction** — Data symbols get direct addresses (no thunk). Function symbols get stubs + gateway. The resolution helper (`udynlink_dep_resolve_func` vs `udynlink_dep_resolve_data`) handles the distinction.

---

## 2.11 Decision Checkpoint

Please confirm or refine:

1. ✅ **Dummy symbol approach confirmed** — `.udynlink.mod.requires.{name}` as EXTERN symbols.
2. ✅ **Two-level dispatch confirmed** — Per-module gateway (16 bytes v7m, 18 bytes v6m) + per-function stub (8 bytes). Saves ~14 bytes per cross-module function reference vs. inline thunks (22/26 bytes). Break-even at N≈1.
3. ✅ **Trampoline in C confirmed** — `__attribute__((naked))` with inline assembly. Easier to maintain than separate `.S` files.
4. ✅ **No core changes confirmed** — Standalone header uses only existing v3.0 APIs.
5. ✅ **Data symbols confirmed** — Direct address resolution, no thunks. Functions need stubs + gateway.
