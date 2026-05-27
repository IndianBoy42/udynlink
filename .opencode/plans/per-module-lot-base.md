# Plan: Per-Module LOT Base Address Refactor — Final Draft

> **Status:** Architecture selected. Awaiting final user sign-off before execution.
> **Chosen Approach:** A — Host-Managed `r9` (eliminate global memory word entirely).
> **ABI Bump:** Loader ABI 2.0 → 3.0. Old modules will require rebuild.

---

## 1. Why Approach A Was Selected

Based on the user's responses:
- **Motivation:** Reentrancy between different modules and separate module systems (bootloader + app, multiple apps).
- **ISR safety:** Required, but Cortex-M exception entry already saves the caller context, and the module prologue saves/restores `r9` on the stack. ISR-safe as long as ISRs use the wrapped entry point.
- **Backward compat:** Willing to break compatibility. Clean ABI bump to 3.0.
- **Performance:** No regressions; actually improves by removing a memory load from every module entry.

---

## 2. Architecture Overview

### 2.1 The Core Change

**Remove the fixed RAM word entirely.**

The host sets `r9 = p_mod->ram_base` directly via inline assembly before every call into module code. The module's assembly prologue no longer loads `r9` from a memory address; it only **saves the caller's `r9` before the call and restores it on return**.

**Current v7m prologue (8 instructions + literal pool):**
```asm
my_func:
    push    {r9, lr}
    push    {r1}
    ldr     r1, .L1my_func
    ldr     r9, [r1]          ; r9 = *(uint32_t *)0x20000000
    pop     {r1}
    bl      __actual_func
    pop     {r9, pc}
.L1my_func:
    .word   0x20000000
```

**New v7m prologue (3 instructions, no literal pool):**
```asm
my_func:
    push    {r9, lr}          ; save caller's r9
    bl      __actual_func     ; r9 already set by host before call
    pop     {r9, pc}          ; restore caller's r9
```

**Current M0 prologue (10 instructions):**
```asm
my_func:
    mov     r2, r9
    push    {r1, r2, lr}
    ldr     r1, .L1
    ldr     r2, [r1]
    mov     r9, r2
    bl      __actual_func
    ldr     r2, [sp, #4]
    mov     r9, r2
    pop     {r1, r2, pc}
.L1:
    .word   0x20000000
```

**New M0 prologue (6 instructions):**
```asm
my_func:
    mov     r2, r9
    push    {r2, lr}          ; save caller's r9 (in r2) and lr
    bl      __actual_func     ; r9 already set by host
    pop     {r2, lr}          ; restore saved r9 into r2
    mov     r9, r2            ; restore caller's r9
    bx      lr
```

### 2.2 Host-Side Changes

**`UDYNLINK_PREPARE_CALL()` becomes a single register move:**
```c
#define UDYNLINK_PREPARE_CALL(p_mod) do { \
    __asm volatile ("mov r9, %0" :: "r"((uint32_t)(p_mod)->ram_base) : "r9"); \
} while(0)
```

This macro now applies to **all** modules (prologued and `--no-prologue`). The distinction disappears at the host level.

**`udynlink_cpp_init()` uses the same macro:**
```c
void udynlink_cpp_init(udynlink_module_t *p_mod) {
    udynlink_sym_t __init_array = {};
    if (udynlink_lookup_symbol(p_mod, "__init_array", &__init_array) != NULL) {
        UDYNLINK_PREPARE_CALL(p_mod);
        typedef void (*void_func)(void);
        void_func f = (void_func)__init_array.val;
        f();
    }
}
```

**C++ `Context` class save/restores the register directly:**
```cpp
explicit Context(const udynlink_module_t *p_mod)
    : p_mod_(p_mod), prev_r9_(0) {
    if (p_mod_ != NULL) {
        __asm volatile ("mov %0, r9" : "=r"(prev_r9_) : :);
        __asm volatile ("mov r9, %0" :: "r"((uint32_t)p_mod_->ram_base) : "r9");
    }
}
~Context() {
    if (p_mod_ != NULL) {
        __asm volatile ("mov r9, %0" :: "r"(prev_r9_) : "r9");
    }
}
```

### 2.3 ISR Safety Detail

The user asked: *"don't cortex M ISRs already save and load registers correctly?"*

**Yes, but with a nuance.**

Cortex-M NVIC exception entry pushes `R0–R3`, `R12`, `LR`, `PC`, and `xPSR` onto the exception stack frame. It does **not** save `R4–R11` (including `R9`). `R9` is a callee-saved register: the *callee* must preserve it if it modifies it.

With Approach A:
- The **module prologue is the callee**. It saves `R9` on its own stack (`push {r9, lr}`) before modifying it, and restores it before returning (`pop {r9, pc}`).
- Therefore, an ISR that calls a module **through the wrapped entry point** is safe: the wrapper saves the interrupted task's `R9`, the module runs with its own `R9`, and the wrapper restores the original `R9` before exception return.
- The only risk is if an ISR **bypasses the wrapper** (e.g., calls a `--no-prologue` function via raw function pointer). In that case, the ISR must manually save/restore `R9`. This is documented in the same way today's docs warn about forgetting to set `UDYNLINK_LOT_BASE_ADDR`.

**Conclusion:** Approach A is ISR-safe for the standard usage pattern (calling wrapped exports).

### 2.4 ABI Version Bump

- `UDYNLINK_LOADER_ABI_VERSION` bumps from `2.0` to `3.0`.
- `mkmodule` defaults `--udynlink-version` to `3.0`.
- A `3.0` host **will reject** modules built with `--udynlink-version 2.0` (or lower) because the header's `udynlink_version` field encodes the minimum required loader version. This is the existing validation logic in `udynlink_validate_header()`.
- The user explicitly accepted this breaking change: old modules must be rebuilt with the new toolchain.

---

## 3. Task Breakdown & Execution Plan

The work is split into **5 atomic tasks**. Tasks 1 and 2 are independent and can be done in parallel. Task 3 depends on 1+2. Task 4 (docs) can be drafted in parallel but must be finalized after validation. Task 5 is purely sequential validation.

### Task A: Core C/C++ Runtime Refactor
**Agent:** `agent` (multi-file C/C++ edits)
**Deliverables:**
1. `udynlink/udynlink.h`:
   - Remove `UDYNLINK_LOT_BASE_ADDR` macro and its `#ifndef` block.
   - Redefine `UDYNLINK_PREPARE_CALL()` to only set `r9` (no memory write).
   - Bump `UDYNLINK_LOADER_ABI_VERSION` to `UDYNLINK_MAKE_VERSION(3, 0)`.
   - Update doc comments for `UDYNLINK_PREPARE_CALL`.
2. `udynlink/udynlink.c`:
   - Rewrite `udynlink_cpp_init()` to use `UDYNLINK_PREPARE_CALL(p_mod)` instead of the manual memory write.
   - Ensure `udynlink_validate_header()` naturally rejects old modules (it already checks `udynlink_version > UDYNLINK_LOADER_ABI_VERSION`).
3. `udynlink/udynlink_call.h`:
   - Remove `UDYNLINK_SET_LOT_BASE` macro.
   - Update `UDYNLINK_SET_R9` doc comment to make it the canonical mechanism.
   - Update `UDYNLINK_CALL` and `UDYNLINK_CALL_MODULE_FUNC` doc comments.
4. `udynlink/udynlink.hpp`:
   - Rewrite `Context` class: `prev_lot_base_` → `prev_r9_` (type `uint32_t`).
   - Constructor reads `r9` into `prev_r9_`, then writes `p_mod_->ram_base` to `r9`.
   - Destructor restores `r9` from `prev_r9_`.
   - `rebind()` method writes `ram_base` to `r9` directly.
   - Update doc comments and warnings.
5. `udynlink/udynlink_externals.h` — verify no references (no changes expected).
6. `udynlink/udynlink_hash.h` — verify no references (no changes expected).

**Review criteria:**
- `UDYNLINK_LOT_BASE_ADDR` string does not appear in any `.h`, `.c`, or `.hpp` file under `udynlink/`.
- `UDYNLINK_PREPARE_CALL` sets `r9` only.
- `udynlink_cpp_init` uses `UDYNLINK_PREPARE_CALL`.
- `Context` compiles with `-mthumb` (inline asm syntax is correct for GCC ARM).

---

### Task B: Build Toolchain Refactor
**Agent:** `agent` (Python + Jinja2 template edits)
**Deliverables:**
1. `scripts/asm_template_armv6m.tmpl`:
   - Remove the literal pool `.word {{lot_base}}` and `.L1{{s}}` label.
   - Remove `ldr r1, .L1{{s}}`, `ldr r2, [r1]`, `mov r9, r2` sequence.
   - Remove `push {r1}` / `pop {r1}` (r1 no longer needed as scratch).
   - Simplify to save caller's `r9` via `r2`, call actual function, restore `r9`.
2. `scripts/asm_template_armv7m.tmpl`:
   - Remove literal pool and `ldr` sequence.
   - Simplify to `push {r9, lr}` / `bl` / `pop {r9, pc}`.
3. `scripts/asm_template_armv8m.tmpl`:
   - Same simplification as v7m.
4. `scripts/asm_template.tmpl` (old fallback template):
   - Same simplification as v7m, or delete if no longer referenced by `targets.py`.
5. `scripts/mkmodule`:
   - Remove `--lot-base` CLI argument and its parsing.
   - Remove `args.lot_base` variable and `lot_base` from Jinja2 template render dict.
   - Change default `--udynlink-version` from `2.0` to `3.0`.
   - Update help text to reflect the new default.
6. `scripts/targets.py`:
   - Verify no `lot_base` references (should be clean).

**Review criteria:**
- `--lot-base` does not appear in `mkmodule --help`.
- Generated prologue `.s` files contain no `.word` literal pool.
- Modules built with the new toolchain have `udynlink_version == 3.0` in the header.
- `just module hello.c` produces a valid binary and the assembly prologue is the simplified form.

---

### Task C: Test Suite Migration
**Agent:** `agent` (C test code edits)
**Deliverables:**
1. Update all `tests/*/test_qemu.c` files that manually write to `UDYNLINK_LOT_BASE_ADDR`:
   - `tests/test-deps/test_qemu.c`
   - `tests/test-streaming-load/test_qemu.c`
   - `tests/test-circular-deps-link/test_qemu.c`
   - `tests/test-optional-dep/test_qemu.c`
   - `tests/test-weak-symbols/test_qemu.c`
   - `tests/test-link-symbol/test_qemu.c`
   - `tests/test-deferred-symbol/test_qemu.c`
   - Any other test files referencing the macro.
   **Change pattern:**
   ```c
   // OLD
   uintptr_t *mod_base = (uintptr_t *)UDYNLINK_LOT_BASE_ADDR;
   *mod_base = p_mod->ram_base;
   
   // NEW
   UDYNLINK_PREPARE_CALL(p_mod);
   ```
2. Update all `tests/platforms/*/platform.cmake`:
   - Remove `UDYNLINK_LOT_BASE_ADDR=0x20000000` from `UDYNLINK_PLATFORM_DEFINES`.
3. Update `tests/platforms/olimex_stm32_h405/mem.ld`:
   - Remove the comment referencing `UDYNLINK_LOT_BASE_ADDR (0x20000000)`.
4. Check `tests/qemu_host/src/main.c` (or any shared host code) for LOT base references.

**Review criteria:**
- `grep -r "UDYNLINK_LOT_BASE_ADDR" tests/` returns zero matches.
- `grep -r "lot_base" tests/platforms/` returns zero matches.
- `just ci` passes (this is validated in Task E).

---

### Task D: Documentation Rewrite
**Agent:** `search` + `agent` (docs)
**Deliverables:**
1. `docs/how-it-works.md`:
   - Rewrite "The LOT Base Address Convention" section to explain that `r9` is set directly by the host before every call, not loaded from a fixed memory word.
   - Update the assembly listings for all three prologue variants.
2. `docs/integrating-as-host.md`:
   - Remove all references to `UDYNLINK_LOT_BASE_ADDR`.
   - Rewrite "Set up the LOT base" instructions to use `UDYNLINK_PREPARE_CALL()`.
   - Update the concurrency/reentrancy section to explain that `r9` is per-call-register state, and the wrapper save/restore makes it safe.
3. `docs/api-reference.md`:
   - Remove `UDYNLINK_LOT_BASE_ADDR` from the API table.
   - Update `UDYNLINK_PREPARE_CALL` docs.
   - Update `udynlink_cpp_init` docs.
   - Update `Context` class docs.
4. `docs/examples.md`:
   - Replace every `uint32_t *lot_base = (uint32_t *)UDYNLINK_LOT_BASE_ADDR;` example with `UDYNLINK_PREPARE_CALL(p_mod);`.
5. `docs/writing-modules.md`:
   - Remove `--lot-base` flag documentation.
   - Remove "Forgot to set the LOT base address" troubleshooting (replace with "Forgot to use UDYNLINK_PREPARE_CALL").
6. `docs/testing.md`:
   - Update test-writing instructions that mention `UDYNLINK_LOT_BASE_ADDR`.
   - Update `UDYNLINK_PLATFORM_DEFINES` documentation.
7. `README.md`:
   - Update quickstart example.
   - Update "How it works" summary.
8. `AGENTS.md`:
   - Remove `UDYNLINK_LOT_BASE_ADDR` from architecture notes and build command descriptions.
   - Update the "Fixed LOT Base" codemap note.

**Review criteria:**
- `grep -r "UDYNLINK_LOT_BASE_ADDR" docs/ README.md AGENTS.md` returns zero matches.
- `grep -r "lot-base" docs/ README.md AGENTS.md` returns zero matches (except perhaps historical changelog references).

---

### Task E: Validation & CI
**Agent:** `shell` + `agent` (testing)
**Deliverables:**
1. Build the core library: `cmake -B build -S . && cmake --build build`.
2. Build and run tests for all platforms via `just`:
   - `just test-mps2` (MPS2-AN386, Cortex-M4)
   - `just test-an385` (MPS2-AN385, Cortex-M3)
   - `just test-an500` (MPS2-AN500, Cortex-M7)
   - `just test-an505` (MPS2-AN505, Cortex-M33)
   - `just test-h405` (Olimex H405, Cortex-M4F)
   - `just test-f429` (STM32F429, legacy QEMU)
3. Verify that every test reports `*** TEST OK ***`.
4. Run `just validate-all-targets` to ensure all 9 targets can still compile `hello.c`.
5. If any test fails, debug and fix (may require revisiting Tasks A–C).

**Review criteria:**
- `just ci` passes with no failures.
- No regressions in test count vs. baseline.

---

## 4. Execution Order & Parallelization

```
[Task A: Core Runtime] ───┐
                          ├──→ [Task C: Test Suite] ──→ [Task E: Validation]
[Task B: Toolchain] ──────┘

[Task D: Docs] ───────────────→ (parallel with A/B/C, finalize after E)
```

- **Phase 1:** Run Task A and Task B in parallel. Both are independent code changes.
- **Phase 2:** Run Task C after A and B complete (tests need the new headers and new prologues to compile).
- **Phase 3:** Run Task E after C completes (requires compiled tests).
- **Phase 4:** Run Task D in parallel with Phase 1–3 for drafting, then finalize after E confirms everything works.

---

## 5. Risk Register

| Risk | Mitigation |
|------|------------|
| M0 prologue inline asm may have subtle push/pop restrictions | Verify disassembly of generated prologue on M0 target via `arm-none-eabi-objdump` before running tests. |
| C++ `Context` inline asm syntax may not compile on all GCC versions | Test with the CI compiler (`gcc-arm-embedded` from GitHub Actions). |
| Some tests may call module functions indirectly (function pointers) and bypass the wrapper | Audit each `test_qemu.c` for raw function pointer calls. If found, ensure they use `UDYNLINK_PREPARE_CALL` before invocation. |
| `udynlink_validate_header()` may already reject v3.0 modules on old hosts, but we need to ensure new hosts handle the version check correctly | Add a unit-style check in `tests/` that validates a v3.0 module header is accepted. (Can be a small assert in an existing test.) |
| Docs may become inconsistent if written before code is final | Draft docs in parallel, but do a final `grep` sweep after all code is locked. |

---

## 6. Decision Checkpoint

**Please confirm the following before we begin execution:**

1. ✅ **Approach A confirmed** — Eliminate `UDYNLINK_LOT_BASE_ADDR` entirely; host sets `r9` directly; prologue only saves/restores `r9`.
2. ✅ **ABI break accepted** — Loader ABI bumps to 3.0. Old modules must be rebuilt.
3. ✅ **ISR safety model accepted** — ISRs calling modules through wrapped exports are safe because the wrapper saves/restores `r9`. ISRs calling `--no-prologue` functions must manually save/restore `r9` (same responsibility as today, just a register instead of a memory word).
4. Should we add a **runtime assertion** in `udynlink_load_module()` that warns if a module header has `udynlink_version < 3.0` with a descriptive error, or is the existing `UDYNLINK_ERR_LOAD_VERSION_MISMATCH` sufficient?
5. Should we delete the old `scripts/asm_template.tmpl` (appears to be an unused duplicate of `asm_template_armv7m.tmpl`), or update it to match?

Once you confirm, I will immediately begin delegating Tasks A–E to implementation agents.
