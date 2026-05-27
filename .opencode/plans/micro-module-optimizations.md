# Plan: Micro-Module Optimizations for udynlink

## 1. Goal

Enable **many tiny, single-function "micro-modules"** to be dynamically loaded and hot-reloaded in embedded firmware with minimal flash/RAM overhead and fast load/unload latency. The primary use case is hook-based firmware extensibility: each module exports a single hook function (e.g. `on_timer`, `on_packet`, `filter_input`) that the host calls dynamically.

**User context:**
- Modules are a mix of pure computation and small host calls (no complex data/bss/dependencies).
- Host already sets LOT base before every call and dispatches via function pointer (`udynlink_lookup_symbol` + cast + call).
- Target scale: **20–50 modules**.
- Hot reload: **stop-world, unload old, load new**.
- Host symbol set: **closed and stable** (~10–20 symbols).

---

## 2. Current Baseline (Quantified)

Measured from existing test modules in this repository (Cortex-M7 / M33, `-Os` / `-O3`):

| Module | `.text` | `.data` | Symbol Table | Relocs | **Total Image** | Notes |
|--------|---------|---------|--------------|--------|-----------------|-------|
| `mod_consumer` (deps test) | 64 B | 0 B | 172 B (8 syms) | 16 B (2) | **288 B** | 1 export, 1 extern (`puts`) |
| `mod_weak` (weak symbols) | 80 B | 4 B | 180 B (8 syms) | 8 B (1) | **308 B** | 2 exports, 1 weak var |
| `mod_local_ptrs` | 208 B | 4 B | 220 B (13 syms) | 48 B (6) | **~520 B** | 2 exports, strings, globals |

### Overhead Breakdown for a Minimal Single-Export Module

For a module with **one exported function** and **no data/bss** (e.g. `int hook(void) { return 42; }`):

| Component | Size | % of Total | Source |
|-----------|------|-----------|--------|
| Header (UDLM + 36-byte metadata) | ~36 B | 12% | Fixed format |
| Symbol table (8 entries incl. 4× `__init_array_*`) | ~172 B | **60%** | Always exported, zero-size |
| Relocations (2 entries: `.LC0` + extern) | ~16 B | 6% | LOT + data reloc |
| Prologue wrapper per exported function | ~28 B | **10%** | `push {r9,lr}` … `pop {r9,pc}` |
| Actual user code | ~4–32 B | 10% | The useful payload |
| **Total** | **~288 B** | | |

**Key insight:** For a minimal micro-module, **~70% of the image is metadata (symbol table + header + relocations) and prologue overhead**, while the actual user code may be only 10–30 bytes.

At the user's scale (20–50 modules), this means:
- **Flash**: ~6–14 KB just for metadata
- **RAM**: One `udynlink_module_t` handle (36 bytes) + LOT/data per module
- **Load latency**: `malloc` + full relocation walk + symbol resolution per module
- **Call latency**: Prologue executes `push/pop` + two `ldr` from fixed memory on every call

---

## 3. Architecture Decisions (Post-Review)

### Decision: Execute Phase 1 Only; Defer Phase 2

With Phase 1 (strip dead symbols + eliminate prologue + symbol cache), a minimal module drops from **288 B → ~150 B**. At 50 modules, that's **~7.5 KB total flash** — well within typical embedded budgets. Phase 2 (numeric symbol IDs) would push this to ~80–100 B per module but requires significant toolchain and API changes. We defer Phase 2 unless Phase 1 proves insufficient in practice.

**Nano-module format (Phase 3) is out of scope.** The user's modules may call host functions, so a format without symbol resolution is too limiting.

### Decision: Additive, Opt-In Optimizations

All changes are **additive** and **opt-in** via new flags. Existing modules and the standard UDLM format remain fully compatible. No ABI breakage.

### Decision: Host-Sets-r9 Is Discoverable at Runtime

The `--no-prologue` mode must not be a "secret handshake" between the toolchain and the host. The module image itself advertises whether it has prologues, so the host can call every module safely through a single universal macro.

**Mechanism:** We repurpose one reserved bit in the `arch_tag` field of the module header:
```c
#define UDYNLINK_ARCH_FLAG_NO_PROLOGUE  0x80  /* bit 15:7 reserved, use bit 7 */
```

When `mkmodule` builds with `--no-prologue`, it sets this bit in `arch_tag`. At runtime the host queries:
```c
static inline int udynlink_module_has_no_prologue(const udynlink_module_header_t *p_header) {
    return (p_header->arch_tag & UDYNLINK_ARCH_FLAG_NO_PROLOGUE) != 0;
}
```

**Ergonomic host-side macro:** This macro works for *both* prologued and non-prologued modules. For prologued modules it behaves exactly like today's code; for no-prologue modules it also sets `r9` directly.
```c
#define UDYNLINK_PREPARE_CALL(p_mod) do { \
    uint32_t *_mb = (uint32_t *)UDYNLINK_LOT_BASE_ADDR; \
    *_mb = (p_mod)->ram_base; \
    if (udynlink_module_has_no_prologue((p_mod)->p_header)) { \
        __asm volatile ("mov r9, %0" :: "r"((p_mod)->ram_base) : "r9"); \
    } \
} while(0)
```

The user's existing call site changes from:
```c
*mod_base = mod.ram_base;
result = hook(arg);
```
to:
```c
UDYNLINK_PREPARE_CALL(&mod);
result = hook(arg);
```

One line, safe for all module types, no runtime mistakes.

---

## 4. Execution Plan: Phase 1 (Three Atomic Tasks)

### Task 1A: Strip Zero-Size `__init_array_*` Symbols from Module Image

**Objective:** Remove the four zero-size exported symbols (`__init_array_start`, `__init_array_end`, `__preinit_array_start`, `__preinit_array_end`) from the emitted symbol table when they have zero size and identical addresses (indicating an empty init array).

**Why this matters:** These symbols consume ~80–100 bytes per module (8-byte table entry + name string × 4). For a minimal module, this is **~30% of the total image size**.

**Implementation approach:**
1. In `scripts/mkmodule`, during symbol table construction in `process()`, detect symbols whose:
   - `bind` is `STB_GLOBAL`
   - `type` is `STT_NOTYPE`
   - `size` is `0x0`
   - name matches `__init_array_*` or `__preinit_array_*`
   - value is identical to another such symbol (empty range)
2. Exclude them from `slist_all` (the list of symbols that go into the image symbol table).
3. Ensure that if a C++ module actually HAS constructors (non-empty init array), the symbols are preserved.

**Files to modify:**
- `scripts/mkmodule` — `process()` function, symbol filtering logic

**Tests:**
- Build `mod_consumer` (`tests/test-deps/`) and verify image size drops by ~80 bytes.
- Run full test suite (`just ci`) — all existing tests must pass unchanged.
- Add a new test `test-strip-init-array/` with a C module and a C++ module, verifying that C modules lose the symbols while C++ modules retain them.

**Deliverables:**
- [ ] `mkmodule` detects and omits empty init-array symbols
- [ ] Image size reduced by ~30% for non-C++ modules
- [ ] New integration test validates the behavior for C vs C++
- [ ] Full CI passes (`just ci`)

**Agent type:** `agent` (multi-file toolchain + test work)
**Estimated effort:** Small (1–2 hours)

---

### Task 1B: Host-Sets-r9 Mode (`--no-prologue`) with Runtime Discovery

**Objective:** Add an optional `--no-prologue` flag to `mkmodule` that skips generating the assembly prologue wrapper for exported functions. The module header advertises this via a flag bit so the host can call both prologued and non-prologued modules safely through a single macro.

**Why this matters:** The prologue is ~28 bytes per exported function plus ~6–8 instructions of call latency. For single-export micro-modules, eliminating it reduces image size by ~10% and improves hook call latency.

**Implementation approach:**

1. **Module header flag (`udynlink.h`):**
   - Define `UDYNLINK_ARCH_FLAG_NO_PROLOGUE 0x80` (repurposes reserved bit 7 of `arch_tag`).
   - Add `udynlink_module_has_no_prologue()` inline helper.
   - Add the universal `UDYNLINK_PREPARE_CALL()` macro (see Architecture Decisions above).

2. **Toolchain (`mkmodule`):**
   - Add `--no-prologue` argument.
   - When active, set `UDYNLINK_ARCH_FLAG_NO_PROLOGUE` in `arch_tag_val`.
   - Skip the Jinja2 template rendering and assembly step for prologues.
   - Do NOT rename exported symbols (skip `objcopy --redefine-sym`).
   - Do NOT make symbols local (skip `objcopy -L`).
   - Exported functions remain global with their original names.

3. **Test harness (`test_utils.c`):**
   - Update `run_test_func()` to use `UDYNLINK_PREPARE_CALL()` instead of the old manual LOT-base write.
   - This makes the harness automatically correct for both prologued and non-prologued modules.

4. **Documentation:**
   - Update `docs/integrating-as-host.md` with `UDYNLINK_PREPARE_CALL` and the new flag.
   - Update `AGENTS.md`.

**Files to modify:**
- `udynlink/udynlink.h` — `UDYNLINK_ARCH_FLAG_NO_PROLOGUE`, inline helpers, macro
- `scripts/mkmodule` — argument parser, set flag bit, skip prologue logic
- `tests/qemu_host/src/test_utils.c` — adopt `UDYNLINK_PREPARE_CALL()` in `run_test_func()`
- `tests/qemu_host/src/test_utils.h` — no new declarations needed if macro is in `udynlink.h`
- `docs/integrating-as-host.md` — document the flag and the macro
- `AGENTS.md` — document the flag

**Tests:**
- Create `test-no-prologue/` with a single-export C module.
- Build with `--no-prologue`.
- Test harness calls it successfully.
- Verify image size is ~28 bytes smaller than the same module built without the flag.
- Verify `udynlink_module_has_no_prologue()` returns true for the loaded module.
- Run full CI (`just ci`) — all existing tests must still pass (they are prologued, so the macro's `if` path is not taken).

**Deliverables:**
- [ ] `mkmodule --no-prologue` sets header flag and skips prologue generation
- [ ] `udynlink_module_has_no_prologue()` runtime query added to public API
- [ ] `UDYNLINK_PREPARE_CALL()` universal macro works for both prologued and non-prologued modules
- [ ] Test harness updated to use the macro
- [ ] New integration test `test-no-prologue/` passes
- [ ] Documentation updated
- [ ] Full CI passes (`just ci`)

**Agent type:** `agent` (toolchain + loader header + test harness + docs)
**Estimated effort:** Medium (2–4 hours)

---

### Task 1C: Host-Side Symbol Resolution Cache Utility

**Objective:** Provide a small, optional, header-only cache utility that the host can drop into its `udynlink_external_resolve_symbol()` callback to avoid repeated `strcmp`/hash lookups when loading multiple modules that reference the same extern symbols. The loader core remains stateless.

**Why this matters:** At 20–50 modules, many will resolve the same host symbols (e.g., `printf`, `malloc`). Currently each module calls the host resolver independently. A cache makes subsequent resolutions O(1) after the first call. By keeping the cache in host code (not the loader core), we preserve the loader's flexibility and zero-footprint philosophy.

**Implementation approach:**

1. **New file: `udynlink/udynlink_host_utils.h`**
   - Pure header-only utility. No `.c` file, no static state inside the loader.
   - Provide a direct-mapped cache struct and helper:
   ```c
   #ifndef UDYNLINK_HOST_SYM_CACHE_SIZE
   #define UDYNLINK_HOST_SYM_CACHE_SIZE 16
   #endif
   
   typedef struct {
       const char *name;
       uintptr_t addr;
   } udynlink_host_sym_cache_entry_t;
   
   static inline uintptr_t udynlink_host_sym_cache_lookup(
       udynlink_host_sym_cache_entry_t *cache,
       size_t cache_size,
       const char *name,
       uintptr_t (*fallback)(const char *name))
   {
       uint32_t h = 0;
       for (const char *p = name; *p; p++) h = h * 31 + (uint8_t)*p;
       size_t idx = h % cache_size;
       if (cache[idx].name && strcmp(cache[idx].name, name) == 0)
           return cache[idx].addr;
       uintptr_t addr = fallback(name);
       if (addr) {
           cache[idx].name = name;
           cache[idx].addr = addr;
       }
       return addr;
   }
   ```

2. **Usage example (for documentation):**
   ```c
   static udynlink_host_sym_cache_entry_t my_cache[UDYNLINK_HOST_SYM_CACHE_SIZE];
   
   uintptr_t udynlink_external_resolve_symbol(const char *name) {
       return udynlink_host_sym_cache_lookup(my_cache, UDYNLINK_HOST_SYM_CACHE_SIZE,
                                             name, my_real_resolver);
   }
   ```

3. **Invalidate helper:**
   ```c
   static inline void udynlink_host_sym_cache_invalidate(
       udynlink_host_sym_cache_entry_t *cache, size_t cache_size) {
       memset(cache, 0, cache_size * sizeof(*cache));
   }
   ```

**Files to modify:**
- `udynlink/udynlink_host_utils.h` — **new file**, all utility code

**Tests:**
- Create `test-sym-cache/` with two modules that both reference the same extern symbol.
- In the test host (`test_qemu.c`), implement `udynlink_external_resolve_symbol()` using the cache utility and a global counter.
- Load module A → counter increments by 1.
- Load module B → counter does **not** increment (cache hit).
- Verify via `test_data.py` regex check on the counter value.
- Run full CI (`just ci`).

**Deliverables:**
- [ ] `udynlink/udynlink_host_utils.h` created with cache lookup + invalidate helpers
- [ ] Header is pure inline / header-only — no loader core changes
- [ ] New integration test `test-sym-cache/` validates cache hit/miss behavior
- [ ] Documentation updated in `docs/integrating-as-host.md` with usage example
- [ ] Full CI passes (`just ci`)

**Agent type:** `agent` (new header + test + docs)
**Estimated effort:** Small–Medium (1–3 hours)

---

## 5. Task Dependencies & Execution Order

```
Task 1A (Strip init-array symbols)
    │
    ├─ Independent: can start immediately
    │
Task 1B (Host-sets-r9 / --no-prologue)
    │
    ├─ Independent of 1A (different code paths)
    ├─ But 1A should merge first to keep CI green on main
    │
Task 1C (Symbol resolution cache)
    │
    ├─ Independent of 1A and 1B (loader-only change)
    ├─ Can run in parallel with 1A and 1B if desired
```

**Recommended order:**
1. **Task 1A** first (smallest, lowest risk, biggest immediate win).
2. **Task 1B** second (medium complexity, but high impact).
3. **Task 1C** third (or parallel with 1B if CI capacity allows).

---

## 6. Out of Scope (Deferred)

| Optimization | Reason for Deferral |
|-------------|---------------------|
| **Numeric symbol IDs (Phase 2)** | Phase 1 achieves ~50% size reduction (288 B → ~150 B). At 50 modules, that's 7.5 KB — acceptable. Numeric IDs require significant toolchain + host API changes. Revisit if Phase 1 is insufficient. |
| **Batch load API** | User scale (20–50) is small enough that individual loads are fine. Batch loading adds API surface for marginal gain at this scale. |
| **In-place reload** | User's hot-reload is stop-world unload+load, which udynlink already supports. In-place reload adds complexity for a use case the user does not need. |
| **Nano-module format** | User's modules call host functions, so a format without symbol resolution is unusable. |
| **Module handle pool / slab allocator** | User provides `udynlink_external_malloc`/`free`. A slab allocator is host-side infrastructure and can be built on top of existing APIs without changing udynlink. |

---

## 7. Context Guide for Execution Agents

When implementing the tasks above, these files are the primary touchpoints:

| File | Role |
|------|------|
| `scripts/mkmodule` | Toolchain entry point. Builds ELF, parses symbols/rels, emits binary image. Key functions: `compile()`, `link()`, `process()`. **Task 1A + 1B.** |
| `scripts/udynlink_utils.py` | ELF parsing helpers: `get_symbols_in_elf()`, `get_relocations_in_elf()`, `get_section_in_elf()`. |
| `scripts/asm_template_*.tmpl` | Jinja2 templates for per-export-function prologue assembly. One per architecture family (armv6m, armv7m, armv8m). **Task 1B only.** |
| `scripts/code_before_data.ld` | Linker script. Defines `__init_array_start/end`. **Read-only for context.** |
| `udynlink/udynlink.c` | Core loader. `udynlink_load_module()`, relocation application, symbol resolution. **Task 1C.** |
| `udynlink/udynlink.h` | Public API and data structures. **Task 1B** (header flag + macro). |
| `udynlink/udynlink_externals.h` | Host callbacks. **Read-only for context.** |
| `udynlink/udynlink_host_utils.h` | Optional host-side utilities (symbol cache). **Task 1C.** |
| `tests/test-*/test_qemu.c` | Host firmware test harness per test case. Shows how modules are loaded and called. |
| `tests/test-*/test_data.py` | Python validation script per test. Defines modules to compile and output regex checks. |
| `tests/qemu_host/src/test_utils.c` | Shared test utilities: `run_test_func()`, `check_exported_symbols()`, etc. **Task 1B.** |
| `tests/qemu_host/src/test_utils.h` | Declarations for test utilities. **Task 1B.** |
| `tests/test_driver.py` | Test orchestrator. Copies test sources, compiles modules via `mkmodule`, builds QEMU ELF, runs it. Understand this to add new tests. |
| `Justfile` | Test runner. `just test-mps2`, `just test-f429-single test-<name>`. |
| `docs/integrating-as-host.md` | Host integration guide. **Task 1B.** |
| `AGENTS.md` | Project conventions and architecture notes. **Task 1B.** |

### Critical Implementation Notes
1. **Symbol table format:** First word = number of entries. Each entry = 2× `uint32_t`: `(name_offset | type_info << 27, value)`. Name strings follow the entries. Local symbols have `name_offset = 0` and no string.
2. **Relocation encoding:** Each relocation = `(lot_offset, symt_offset)` pair of `uint32_t`. `symt_offset` has bit 31 set for `R_ARM_ABS32` data relocs, bit 30 for `R_ARM_TARGET1`.
3. **Prologue ABI:** The prologue loads `r9` from `UDYNLINK_LOT_BASE_ADDR` (default `0x20000000`). The host must write `p_mod->ram_base` there before calling. `r9` points to the LOT (first `num_lot × 4` bytes of RAM).
4. **Architecture templates:** `asm_template_armv6m.tmpl` (Cortex-M0/M0+), `asm_template_armv7m.tmpl` (M3/M4/M7), `asm_template_armv8m.tmpl` (M33/M55/M85). They differ in instruction availability (e.g., `ldr.w` vs `ldr`).
5. **Tests run twice:** Every test is built and run with `-O0` and `-Os` (or `-O3`). Any change to the toolchain must not break either optimization level.
6. **Load modes:** `COPY_ALL` (header+code+data in RAM), `COPY_TEXT_DATA` (code+data in RAM), `XIP` (only data in RAM). Streaming load only supports `COPY_ALL`/`COPY_TEXT_DATA`.
7. **Test isolation:** `test_driver.py` creates isolated `build_<platform>_<test>_<opt>_src` directories for each test. It copies all test source files there before compiling. New tests need a `test_data.py` and `test_qemu.c`.

---

## 8. Review Checklist (Post-User-Review)

- [x] **Q1 answered:** Module type = mix of pure computation + small host calls
- [x] **Q2 answered:** Host calls via function pointer after setting LOT base (standard pattern)
- [x] **Q3 answered:** Scale = 20–50 modules
- [x] **Q4 answered:** Hot reload = stop-world unload+load
- [x] **Q5 answered:** Host symbol set = closed and stable
- [x] **Decision made:** Execute Phase 1 only (1A + 1B + 1C); defer Phase 2
- [x] **Decision made:** Nano-module format out of scope
- [x] **User sign-off:** Approved (plan refined after two review rounds)

---

## 9. Execution Complete ✅

All three tasks have been implemented, tested, and committed. Full CI (`just ci`) passes on all 5 platforms with **44/44 tests** (MPS2-AN386/385/500/505 + Olimex H405).

### Deliverables Checklist

**Task 1A — Strip Zero-Size Init-Array Symbols**
- [x] `mkmodule` detects and omits empty init-array symbols
- [x] Image size reduced by ~30% for non-C++ modules (288 B → 176 B)
- [x] New integration test `test-strip-init-array/` validates C vs C++ behavior
- [x] Full CI passes

**Task 1B — Host-Sets-r9 Mode (`--no-prologue`) with Runtime Discovery**
- [x] `mkmodule --no-prologue` sets header flag and skips prologue generation
- [x] `udynlink_module_has_no_prologue()` runtime query added to public API
- [x] `UDYNLINK_PREPARE_CALL()` universal macro works for both prologued and non-prologued modules
- [x] Test harness updated to use the macro
- [x] New integration test `test-no-prologue/` passes (all 3 load modes)
- [x] Documentation updated in `docs/integrating-as-host.md` and `AGENTS.md`
- [x] Full CI passes

**Task 1C — Host-Side Symbol Resolution Cache Utility**
- [x] `udynlink/udynlink_host_utils.h` created with cache lookup + invalidate helpers
- [x] Header is pure inline / header-only — no loader core changes
- [x] New integration test `test-sym-cache/` validates cache hit/miss behavior
- [x] Documentation updated in `docs/integrating-as-host.md`
- [x] Full CI passes

### Commits

| Commit | Description |
|--------|-------------|
| `nukzwnso` | Strip empty init-array sentinel symbols from C module symbol table |
| `wwovqtst` | feat: add `--no-prologue` flag with runtime discovery via `arch_tag` bit |
| `slpqlyon` | feat: add `udynlink_host_utils.h` header-only symbol cache utility |

### Impact Summary

For a minimal single-export micro-module (C, `-O3`, Cortex-M4):

| Optimization | Size | Delta |
|--------------|------|-------|
| Baseline (before any changes) | **288 B** | — |
| After Task 1A (strip init-array) | **176 B** | **–112 B (39%)** |
| After Task 1B (`--no-prologue`) | **84 B** | **–92 B (52% from 176 B)** |

**Total reduction: 288 B → 84 B (71% smaller)**

At the user's scale of 20–50 modules, flash metadata drops from **~6–14 KB** to **~1.7–4.2 KB**.

---

*Plan version: 2.2 (execution complete)*
*Date: 2026-05-27*
