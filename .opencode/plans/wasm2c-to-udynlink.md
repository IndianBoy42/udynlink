# Plan: wasm2c → udynlink Module Compilation

## Goal
Determine feasibility and, if viable, implement a toolchain that compiles a WebAssembly binary module (`.wasm`) into a udynlink-compatible loadable module for ARM Cortex-M via `wasm2c` + ARM GCC.

## Executive Summary

**Yes, it is possible.** The technical barriers are surmountable with targeted integration work. The main challenges are:
1.  Compiling wasm2c's generated C with udynlink's strict PIC flags (`-fPIE`, `r9` base register).
2.  Providing a bare-metal-compatible `wasm2c` runtime (replacing `setjmp`/`longjmp` and `malloc` dependencies).
3.  Bridging the wasm2c "instance" model with udynlink's load-and-call model.

This plan outlines a proof-of-concept path and asks the user to clarify scope and constraints.

---

## 1. Context: What We Are Bridging

### 1.1 udynlink Requirements (from codebase analysis)

| Requirement | Details |
|-------------|---------|
| **Target** | ARM Cortex-M (M0/M0+/M3/M4/M4F/M7/M33/M55/M85) |
| **PIC Model** | `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative -mlong-calls` |
| **Base Register** | `r9` reserved; host must set `r9 = p_mod->ram_base` before every call |
| **Linker** | `-nostartfiles -nodefaultlibs -nostdlib -Wl,--emit-relocs,--gc-sections` |
| **Format** | Custom binary (`UDLM` header, LOT, relocs, symtab, .text, .data) |
| **Relocations** | `R_ARM_GOT_BREL` (LOT), `R_ARM_ABS32`/`R_ARM_TARGET1` (data), ignored `R_ARM_THM_CALL` |
| **Host Hooks** | `udynlink_external_malloc/free`, `udynlink_external_resolve_symbol`, `udynlink_external_vprintf` |
| **Runtime** | No libc/CRT; host provides all external symbols |

### 1.2 wasm2c Output Model (from WABT docs/source)

| Aspect | Details |
|--------|---------|
| **Output** | Plain C99/C11 source + header |
| **Instance Model** | All state (memory, globals, tables) lives in a `w2c_<modname>` struct |
| **Exports** | Functions like `w2c_modname_funcname(w2c_modname* instance, ...)` |
| **Runtime** | Requires `wasm_rt_allocate_memory`, `wasm_rt_trap`, `memset`, `memcpy`, etc. |
| **Traps** | Default uses `setjmp`/`longjmp`; **overridable via `WASM_RT_TRAP_HANDLER`** |
| **Memory** | Linear memory allocated via `wasm_rt_allocate_memory` (calls `malloc` in default runtime) |
| **Optimization** | Requires `-fno-optimize-sibling-calls -frounding-math` (and maybe `-fsignaling-nans` on GCC) |

### 1.3 The Compatibility Matrix

| Component | Compatibility | Notes |
|-----------|-------------|-------|
| **C99/C11 source output** | ✅ Compatible | Compiles with `arm-none-eabi-gcc` |
| **PIC flags (`-fPIE`, `r9`)** | ⚠️ **Needs verification** | Must compile wasm2c generated code + runtime with udynlink PIC flags. Standard `-fPIC` is **not** the same as `-fPIE -msingle-pic-base`. |
| **GOT/LOT indirection** | ✅ Compatible | wasm2c accesses globals via the instance struct; GCC will route static globals through LOT if PIC flags are correct. |
| **Function pointer tables (`CALL_INDIRECT`)** | ✅ Compatible | Stored in `.data`; udynlink's `R_ARM_ABS32` relocation fixes function pointers at load time. |
| **Data relocations** | ✅ Compatible | Absolute pointers in `.data` are patched by loader. |
| **Linear memory (`wasm_rt_memory_t`)** | ⚠️ **Needs custom runtime** | Default runtime calls `malloc`. Must replace with `udynlink_external_malloc` or pre-allocate in module data. |
| **Trap handling (`wasm_rt_trap`)** | ⚠️ **Needs custom runtime** | Default uses `setjmp`/`longjmp` (requires libc). Must define `WASM_RT_TRAP_HANDLER` with a bare-metal handler (e.g., return error code, or `udynlink_external_vprintf` + halt). |
| **Runtime functions (`memset`, `memcpy`)** | ⚠️ **Host must provide** | These are extern symbols resolved via `udynlink_external_resolve_symbol`. The host firmware must export them. |
| **Instance initialization (`*_instantiate`)** | ⚠️ **Needs integration** | wasm2c requires calling an init function before use. This is analogous to C++ `udynlink_cpp_init`. Host must call it after `udynlink_load_module`. |
| **Module size / symbol count** | ⚠️ **Potential limit** | udynlink uses 16-bit fields for `num_lot` and `num_rels` (max 65535). Large wasm modules may exceed this. |
| **No standard library** | ⚠️ **Must strip** | wasm2c default runtime includes `<stdlib.h>`, `<string.h>`, `<setjmp.h>`. These must be removed or replaced. |

---

## 2. Architecture Options

### Option A: Custom Bare-Metal wasm2c Runtime (Recommended)

Create a replacement for `wasm-rt-impl.c` / `wasm-rt-impl.h` that is bare-metal friendly and uses udynlink host callbacks.

**Key pieces:**
1.  **Memory allocation:** Implement `wasm_rt_allocate_memory` using `udynlink_external_malloc` (and `udynlink_external_free`).
2.  **Trap handling:** Compile with `-DWASM_RT_TRAP_HANDLER=my_trap_handler`. The handler logs the error (via `udynlink_external_vprintf`) and returns control (since `noreturn` is tricky on bare metal without OS process model, we may need a custom `wasm_rt_unreachable()` that halts or returns a sentinel).
3.  **String/memory ops:** Provide `memset`/`memcpy` from the host firmware or compile `newlib` `memset` into the module.
4.  **No `setjmp`:** Remove all `jmp_buf` usage by providing the trap handler macro.

**Pros:** Clean separation; wasm2c generated code is unchanged.  
**Cons:** Requires writing and maintaining a parallel runtime.

### Option B: Static Linear Memory (Simpler, Less Flexible)

Instead of `wasm_rt_allocate_memory`, modify the wasm2c output (or pre-process it) to embed the linear memory as a `.bss` array inside the instance struct. This avoids dynamic allocation entirely.

**Pros:** No `malloc` dependency; simpler.  
**Cons:** Requires either (a) patching wasm2c source, or (b) post-processing generated C to replace `wasm_rt_allocate_memory` with static initialization. Wasm memory max must be known at build time.

### Option C: Full wasm2c Runtime Compiled into Module

Compile the standard `wasm-rt-impl.c` into the module with PIC flags, and let the linker resolve `malloc` → `udynlink_external_malloc` via the host symbol table.

**Pros:** Minimal changes to existing files.  
**Cons:** `setjmp`/`longjmp` is the blocker — bare-metal ARM GCC has `setjmp` in newlib but it may pull in unwanted libc baggage. Also, the standard runtime includes OS-specific signal handlers. This is **not recommended** for a clean solution.

---

## 3. Proposed Proof-of-Concept (PoC) Steps

### Phase 1: Feasibility Spike (No file changes yet)
1.  **Select a tiny wasm module** (e.g., `fac.wasm` or a simple `add.wasm`).
2.  **Generate C with wasm2c:** `wasm2c tiny.wasm -o tiny.c`
3.  **Audit generated C:** Verify it has no inline assembly, no hardcoded addresses, no x86-specific intrinsics (segue).
4.  **Create bare-metal runtime stub:** A minimal `wasm-rt-impl.c` replacement that defines:
    *   `wasm_rt_allocate_memory` (calls `udynlink_external_malloc`)
    *   `wasm_rt_trap` (calls `udynlink_external_vprintf` then halts/returns)
    *   `memset`, `memcpy` (host-resolved or local implementation)
    *   Compile with `-DWASM_RT_TRAP_HANDLER=my_trap_handler`
5.  **Compile with udynlink flags:** Use `arm-none-eabi-gcc -fPIE -msingle-pic-base -mno-pic-data-is-text-relative ...` and inspect ELF for `R_ARM_GOT_BREL` and `R_ARM_ABS32`.
6.  **Run through `mkmodule`:** Attempt to build a `.bin` module. Check if `mkmodule` handles the symbols correctly.

### Phase 2: Integration (If Phase 1 succeeds)
1.  **Write a `wasm2c` bare-metal runtime** (`wasm2c/wasm-rt-udynlink.c` / `.h`) in the repo.
2.  **Write a wrapper script** (`scripts/mkwasm2c-module`) that:
    *   Runs `wasm2c` on input `.wasm`
    *   Compiles generated `.c` + bare-metal runtime with udynlink flags
    *   Runs `mkmodule`-equivalent logic to produce the final `.bin`
3.  **Write a QEMU test case:** A host firmware that loads the wasm-derived module, instantiates it, calls an exported function, and verifies the result.

### Phase 3: Hardening
1.  Handle `CALL_INDIRECT` function tables.
2.  Handle multiple memories (if needed).
3.  Address the 16-bit `num_rels`/`num_lot` limit for large modules.
4.  Optimize: static linear memory option, dead-code stripping.

---

## 4. User Requirements (Clarified)

| Question | Answer |
|----------|--------|
| **Q1: Use case** | **B** — Build-time compilation toolchain (`mkmodule`-style) that accepts `.wasm` instead of C source. |
| **Q2: Trap handling** | **Reject/fatal** — Traps are not recoverable. The toolchain can reject wasm modules with trap instructions at compile time, or the runtime halts/abort on trap. No `setjmp`/`longjmp` needed. |
| **Q3: Linear memory** | **B if no `memory.grow`; A if `memory.grow` exists** — Start with static `.bss` for PoC; add dynamic allocation later. |
| **Q4: Wasm features** | **Core MVP** — No SIMD, threads, exceptions, multi-memory for now. |
| **Q5: Target** | **All Cortex-M targets** — Design for portability; PoC on a single target (Cortex-M4 recommended). |

---

## 5. Updated Architecture

### Chosen Approach: Custom Bare-Metal Runtime + Static Memory (Phase 1)

Since traps are fatal and we start with MVP, we can build a minimal runtime that:
1.  **Omits `setjmp`/`longjmp` entirely** — `wasm_rt_trap()` calls `udynlink_external_vprintf()` and then loops forever (or calls a host-provided abort).
2.  **Uses static linear memory** for PoC — embed the wasm memory as a `.bss` array in the instance struct. This avoids `malloc` and `udynlink_external_malloc` entirely for the first iteration.
3.  **Provides `memset`/`memcpy`** as local, inlined, or host-resolved symbols.
4.  **Compiles the generated C + runtime with udynlink PIC flags** and feeds it into the existing `mkmodule` toolchain.

### Memory Model: Static (Phase 1)

```c
typedef struct {
  // ... existing wasm2c instance fields (globals, tables)
  wasm_rt_memory_t w2c_memory;
  // Static linear memory buffer embedded directly in the instance struct
  // The instance struct itself is allocated in the module's .bss
} w2c_modname;
```

The `wasm_rt_allocate_memory` stub simply sets `mem->data` to point to the pre-allocated buffer and sets `size`/`pages`. No actual allocation occurs.

### Memory Model: Dynamic (Phase 2)

If the wasm module uses `memory.grow`, replace the static buffer with:
```c
void wasm_rt_allocate_memory(...) {
  mem->data = udynlink_external_malloc(initial_pages * page_size);
  // ... set size, pages
}
```
And implement `wasm_rt_grow_memory` using `udynlink_external_realloc` (or malloc+memcpy+free if realloc unavailable).

---

## 6. Risks & Mitigations

| Risk | Likelihood | Mitigation |
|------|------------|------------|
| wasm2c generated C fails to compile with `-fPIE -msingle-pic-base` | Medium | **Spike first.** If it fails, try `-fPIC` + `-shared -symbolic` as a workaround (per ARM community findings). |
| `R_ARM_ABS32` in `.text` section (unhandled by mkmodule) | Low | Audit generated assembly. If present, extend `mkmodule` or linker script. |
| `num_rels`/`num_lot` 16-bit overflow for non-trivial wasm | Medium | Only affects large modules. MVP modules are small. |
| PIC performance overhead on Cortex-M0 | Low | Expected ~1 extra load per global access. Acceptable. |
| wasm2c generated code calls `memset`/`memcpy` on large blocks | Low | Provide local implementations or host hooks. |

---

## 7. Execution Plan & Task Breakdown

### Phase 1: Feasibility Spike
**Goal:** Verify that a tiny wasm module can be converted to a working udynlink module.

**Tasks:**
1.  **Install `wasm2c` / WABT** in the environment.
2.  **Create a tiny test wasm module** (`add.wasm` or `fac.wasm`).
3.  **Generate C** with `wasm2c`.
4.  **Write a minimal bare-metal runtime stub** (`wasm-rt-udynlink.c`):
    *   `wasm_rt_allocate_memory` → sets `data` pointer to static buffer
    *   `wasm_rt_trap` → fatal loop
    *   `memset`, `memcpy` → simple local implementations
    *   No `setjmp`, no `malloc`, no `<stdlib.h>`
5.  **Compile with udynlink PIC flags** and inspect relocations (`R_ARM_GOT_BREL`, `R_ARM_ABS32`).
6.  **Run through `mkmodule`** to produce a `.bin`.
7.  **Write a minimal QEMU host test** that loads the module, instantiates, calls an export, and checks the result.

**Deliverable:** A working end-to-end PoC with one tiny wasm module on one QEMU platform.

### Phase 2: Toolchain Integration
**Goal:** Build a reusable `mkwasm2c-module` script.

**Tasks:**
1.  **Refine the bare-metal runtime** into a proper `wasm-rt-udynlink.h` / `.c` with both static and dynamic memory options.
2.  **Write `scripts/mkwasm2c-module`** — a Python script (or shell) that:
    *   Accepts `.wasm` + target + options
    *   Runs `wasm2c`
    *   Compiles generated `.c` + runtime + udynlink flags
    *   Invokes `mkmodule`-equivalent logic to output `.bin` and optionally C header
3.  **Add QEMU test cases** for:
    *   `add.wasm` (no memory)
    *   `fac.wasm` (no memory, recursion)
    *   `hello.wasm` (linear memory, data segment)
    *   `memory_grow.wasm` (dynamic memory, Phase 2.5)
4.  **Validate all Cortex-M targets** via `just validate-all-targets` equivalent.

**Deliverable:** A production-ready toolchain script and passing tests.

### Phase 3: Hardening & Extensions
**Goal:** Support more wasm features and edge cases.

**Tasks:**
1.  Support `memory.grow` (dynamic memory allocation via host `malloc`).
2.  Support bulk memory / sign-extension (wasm2c already generates C for these; mainly compile flags).
3.  Handle large modules (address 16-bit `num_rels`/`num_lot` limit if hit).
4.  Optimize dead code stripping (`--gc-sections` effectiveness on wasm2c output).
5.  Document the toolchain in `docs/wasm2c-integration.md`.

**Deliverable:** Full documentation and feature-complete toolchain.

---

## Phase 1 Results (Completed)

**Status: ✅ SUCCESS** — The feasibility spike proved that a tiny wasm module can be compiled into a working udynlink loadable module.

### What was accomplished
- Created `add.wat` → assembled to `add.wasm` → generated `add.c`/`add.h` via `wasm2c`
- Wrote a minimal bare-metal runtime stub (`wasm-rt-udynlink.c` + `wasm-rt.h`) with:
  - Fatal trap handler (infinite loop, no `setjmp`/`longjmp`)
  - Static linear memory (pre-allocated `.bss` buffer)
  - `wasm_rt_memcpy` via compiler builtin
  - No libc dependencies
- Successfully compiled through `scripts/mkmodule` into `mod_wasm2c_add.bin`
- QEMU tests **passed** on:
  - **MPS2-AN386** (mainline QEMU 9.2.4, Cortex-M4)
  - **STM32F429** (legacy xPack QEMU 7.2.5, Cortex-M4)
- Both `-O3` and `-Os` builds pass (4/4 test runs)

### Key technical findings
1. **PIC compatibility is clean** — wasm2c-generated code accesses globals via the instance struct; compiled with `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative`, GCC emits `R_ARM_GOT_BREL` relocations that udynlink's loader patches correctly into the LOT at `r9` base.
2. **Dead-code elimination is essential** — wasm2c emits ~700 lines of boilerplate for a 3-instruction function. With `-ffunction-sections -fdata-sections -Wl,--gc-sections`, the final `.text` is stripped to ~424 bytes (actual `add` logic is a single `adds` instruction).
3. **No stdlib dependency achieved** — `-DNDEBUG` neutralizes `assert`, compiler builtins replace `memcpy`, and `--gc-sections` strips unused math helpers / `va_list` code.
4. **Prologue wrappers work transparently** — `mkmodule` auto-generated assembly prologues save/restore `r9` around module calls, so the host doesn't need manual `UDYNLINK_PREPARE_CALL`.
5. **Minimal runtime is sufficient for MVP** — Only `wasm_rt_is_initialized()`, `wasm_rt_trap()`, and `wasm_rt_memcpy()` were actually needed for a memory-less module.

### Issues encountered and resolved
| Issue | Resolution |
|-------|------------|
| `NULL` undeclared in custom header | Added `#include <stddef.h>` |
| `assert()` pulled in libc `__assert_func` | Prepended `#define NDEBUG` to wasm2c output |
| `va_arg` promotion warnings in dead code | Harmless; stripped by `--gc-sections` |
| Test harness regex mismatch | Adjusted `test_data.py` pattern for single-test output |

### Files created in the spike
- `tests/wasm2c_poc/add.wat`, `add.wasm`, `add.c`, `add.h` — source and generated code
- `tests/test-wasm2c-add/wasm-rt.h` — minimal bare-metal runtime header
- `tests/test-wasm2c-add/wasm-rt-udynlink.c` — runtime stub implementation
- `tests/test-wasm2c-add/add.c`, `add.h` — patched wasm2c output
- `tests/test-wasm2c-add/mod_wasm2c_add.c` — module wrapper (instantiate + call export)
- `tests/test-wasm2c-add/test_qemu.c` — host firmware test
- `tests/test-wasm2c-add/test_data.py` — test harness metadata

---

## Updated Status & Next Steps

| Phase | Status | Action |
|-------|--------|--------|
| Phase 1: Feasibility Spike | ✅ **DONE** | — |
| Phase 2: Toolchain Integration | ⏳ **READY** | Write `scripts/mkwasm2c-module`; add tests for `fac`, `hello` (data segments), `memory_grow` |
| Phase 3: Hardening | ⏳ **PENDING** | Dynamic memory, bulk memory, documentation |

---

---

## 8. Full Roadmap: Wasm Feature Support for Cortex-M

### Design Philosophy: udynlink Is Just a Linker

The core `wasm-rt-udynlink` runtime must stay **lean, flexible, and unopinionated**. It is not a full WebAssembly runtime — it is a thin adapter that lets wasm2c-generated C code link against the user's existing firmware.

**Core principle:** The runtime defines the interface; the user's firmware provides the implementation via hooks and callbacks. The user controls:
- How memory is allocated (static buffer, heap, pool, etc.)
- What happens on trap (halt, reset, log, or return an error code)
- How host functions are resolved (symbol table, hardcoded, dynamic)
- Whether features like threads or exceptions are supported (compile-time flags)

This mirrors udynlink's existing model: `udynlink_external_malloc`, `udynlink_external_resolve_symbol`, etc. are user-provided callbacks.

### Runtime Hook Interface

The user provides these callbacks (declared weak so default no-op/fatal stubs exist):

| Hook | Signature | Default | Purpose |
|------|-----------|---------|---------|
| `wasm_rt_malloc` | `void* (size_t)` | `udynlink_external_malloc` | Allocate linear memory, tables, etc. |
| `wasm_rt_free` | `void (void*)` | `udynlink_external_free` | Free linear memory, tables |
| `wasm_rt_realloc` | `void* (void*, size_t)` | `malloc+memcpy+free` | Memory grow |
| `wasm_rt_trap_handler` | `void (wasm_rt_trap_t)` | Infinite loop | Trap policy: user decides |
| `wasm_rt_resolve_import` | `void* (const char* module, const char* name)` | `udynlink_external_resolve_symbol` | Resolve imported functions |
| `wasm_rt_vprintf` | `int (const char* fmt, va_list)` | `udynlink_external_vprintf` | Debug output |
| `wasm_rt_get_time_ms` | `uint32_t (void)` | `0` | Optional: for `clock_time_get` |
| `wasm_rt_sleep_ms` | `void (uint32_t)` | No-op | Optional: yield/sleep |
| `wasm_rt_memory_protection_check` | `void (wasm_rt_memory_t*, u64 addr, u64 n)` | No-op | Optional: guard pages / MPU |

### WebAssembly Feature Priority Matrix

| # | Feature | Priority | Rationale | Implementation Strategy |
|---|---------|----------|-----------|------------------------|
| 1 | **Core MVP** | ✅ **DONE** | Foundation | Already working |
| 2 | **Bulk memory** (`memory.copy`, `memory.fill`, `memory.init`, `data.drop`) | **HIGH** | Rust/Zig emit this by default. Critical for data-heavy modules. | wasm2c generates `memcpy`/`memset` calls. Already works if host provides them. Just need to verify dead-code stripping doesn't remove them. |
| 3 | **Sign-extension** (`i32.extend8_s`, etc.) | **HIGH** | Very common in modern toolchains. Trivial C code. | wasm2c already generates plain C. Just pass `--enable-sign-extension` or rely on default. No runtime work needed. |
| 4 | **Mutable globals** | **HIGH** | Already in MVP by default. Used for module state. | Already supported. wasm2c puts globals in the instance struct. |
| 5 | **Non-trapping float-to-int** | **MEDIUM** | Common in modern toolchains. Avoids trap branches. | wasm2c generates C with saturating conversions. No runtime needed. |
| 6 | **Multi-value** | **MEDIUM** | Enables functions returning multiple values (e.g., `(i32, i32)`). Rust uses this. | wasm2c generates struct returns. C ABI handles it. No runtime needed. |
| 7 | **Tail-call** | **MEDIUM** | Functional languages (Lisp/Scheme→Wasm) use this. Prevents stack exhaustion. | wasm2c generates `goto` or regular calls with `-fno-optimize-sibling-calls`. May need to relax that flag for true tail calls. |
| 8 | **Custom page sizes** | **MEDIUM** | Allows memory smaller than 64KiB. Useful on memory-constrained MCUs (e.g., 4KiB heap). | Replace `WASM_DEFAULT_PAGE_SIZE` at compile time. Runtime already uses `page_size` field. |
| 9 | **Multi-memory** | **LOW** | Separates stack, heap, data into distinct memories. Could be useful for MPU isolation. | Requires multiple `wasm_rt_memory_t` instances in the instance struct. Runtime already supports this. |
| 10 | **Extended const** | **LOW** | Better constant expressions in global initializers. | Already supported by wasm2c. No runtime work. |
| 11 | **SIMD** (`v128`) | **NOT APPLICABLE** | Cortex-M has no vector units (except M55/M85 Helium, which is niche). | **Skip.** Even if supported, the performance gain on 32-bit scalar MCUs is minimal vs. code size cost. |
| 12 | **Threads / atomics** | **NOT APPLICABLE** | Bare-metal Cortex-M has no OS threads. Only interrupt contexts. Atomic ops can be done with `cpsid`/`cpsie` but the wasm thread model doesn't map. | **Skip.** If needed later, implement via single-threaded fake atomics (all sequentially consistent). |
| 13 | **Exceptions** (`try`/`catch`/`throw`) | **NOT APPLICABLE** | Requires heavy runtime (`setjmp`-like unwinding) and is rarely used in embedded Rust/Zig. | **Skip.** The `panic=abort` model (trap on panic) is the embedded default. |
| 14 | **Memory64** (`i64` addresses) | **NOT APPLICABLE** | 64-bit pointers on a 32-bit MCU is wasteful and unnecessary. | **Skip.** Cortex-M address space is 32-bit. |
| 15 | **Reference types / GC** | **NOT APPLICABLE** | Managed references and garbage collection are irrelevant for bare-metal C/Rust firmware. | **Skip.** |
| 16 | **Function references** | **LOW** | Typed function references. Improves type safety of indirect calls. | wasm2c supports it. May add minor table size overhead. Low priority. |
| 17 | **Wide arithmetic** (`i64.*_wide`) | **LOW** | 128-bit arithmetic for crypto. Niche on Cortex-M. | Only if user specifically needs it. |

### Runtime Feature Flags (Compile-Time)

The user controls what runtime support is compiled into the module via `-D` flags passed through `mkwasm2c-module`:

```bash
mkwasm2c-module --enable-bulk-memory --enable-custom-page-sizes=4096 \
    --trap-handler=my_trap --malloc=my_malloc \
    input.wasm -o output.bin
```

| Flag | Effect |
|------|--------|
| `-DUDYNLINK_WASM_TRAP_HANDLER=my_handler` | Override `wasm_rt_trap` behavior |
| `-DUDYNLINK_WASM_MALLOC=my_malloc` | Override memory allocator |
| `-DUDYNLINK_WASM_CUSTOM_PAGE_SIZE=N` | Set wasm page size (default 65536) |
| `-DUDYNLINK_WASM_ENABLE_BULK_MEMORY` | Ensure `memcpy`/`memset` are preserved from `--gc-sections` |
| `-DUDYNLINK_WASM_ENABLE_MULTI_MEMORY` | Reserve space for multiple `wasm_rt_memory_t` structs |
| `-DUDYNLINK_WASM_NO_MPU_CHECKS` | Disable optional `memory_protection_check` hook |

### Estimated Timeline

| Phase | Features | Effort | Est. Time |
|-------|----------|--------|-----------|
| **Phase 2** | Toolchain script (`mkwasm2c-module`); `fac`, `hello` tests; static + dynamic memory modes; bulk memory validation | Medium | 1–2 sessions |
| **Phase 2.5** | Dynamic memory (`memory.grow`); `wasm_rt_realloc` hook; `memory_grow` test | Small | 1 session |
| **Phase 3** | Multi-value, tail-call, custom page sizes, non-trapping float-to-int tests | Medium | 1 session |
| **Phase 4** | Multi-memory support; MPU isolation hooks; advanced test cases | Large | 2 sessions |
| **Phase 5** | Documentation (`docs/wasm2c-integration.md`); CI integration; Justfile targets | Small | 1 session |

---

*Plan created: 2026-05-28*  
*Phase 1 completed: 2026-05-28*  
*Status: Ready for Phase 2*
