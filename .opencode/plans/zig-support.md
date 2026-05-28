# udynlink Zig Support — Master Plan

## Goal

Add first-class Zig support to udynlink on both sides of the loader boundary:

1. **Zig as Host** — A Zig package that wraps the C loader with idiomatic Zig APIs
   (error unions, comptime type-safe calls, slice-based memory management).

2. **Zig as Module** — A toolchain and guide for compiling Zig source into
   position-independent UDLM images that can be loaded at runtime, with FFI
   patterns for importing host symbols and exporting module functions.

## Current State & Context (for future agents)

### Core Loader (`udynlink/`)
- **Public API**: `udynlink.h` — 733 lines, pure C, no stdlib deps.
- **Host callbacks**: `udynlink_externals.h` — 4 weak functions the host must
  implement (`malloc`, `free`, `vprintf`, `resolve_symbol`).
- **Call ergonomics**: `udynlink_call.h` — `UDYNLINK_PREPARE_CALL()` sets `r9` to
  the module's LOT base via inline asm. `UDYNLINK_CALL()` saves/restores `r9`
  around a variadic function-pointer cast.
- **Hash resolution**: `udynlink_hash.h` — optional O(1) GNU hash table for host
  symbol resolution.
- **C++ support**: `udynlink_cpp_init()` runs `__init_array` constructors.

### Toolchain (`scripts/`)
- **`mkmodule`** (Python 3, ~564 lines): compiles C/C++ → ELF → UDLM binary.
  - Compile flags: `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative`
  - Linker: `-T code_before_data.ld -nostartfiles -nodefaultlibs -nostdlib`
  - Prologue generation: Jinja2 templates (`asm_template_armv6m.tmpl`,
    `asm_template_armv7m.tmpl`, `asm_template_armv8m.tmpl`) wrap exported
    functions in `push {r9, lr} / bl <wrapped> / pop {r9, pc}`.
  - Relocations parsed with `pyelftools`. Handles `R_ARM_GOT_BREL` (LOT),
    `R_ARM_ABS32`/`R_ARM_TARGET1` (data), and ignores `R_ARM_THM_CALL`.
- **`targets.py`**: Database of 9 Cortex-M targets (M0 → M85) with `arch_tag`,
  FPU, float ABI, and per-arch template selection.

### Binary Format
- Signature `UDLM`, 32-byte header with `mod_version`, `udynlink_version`, `arch_tag`.
- Layout: [Header] [Relocs] [Symtab] [Code] [Data].
- LOT (Linker Offset Table) lives at the start of the module's RAM region.
- Data accesses are `r9`-relative. Exported functions get an assembly prologue
  that saves/restores the caller's `r9`.

### Testing
- QEMU-based integration tests under `tests/`.
- `tests/qemu_host/CMakeLists.txt` builds a host ELF that links `udynlink.c`.
- Per-test: `test_qemu.c` + `test_data.py` + module C sources.
- `just` commands run the suite for each platform (`just test-mps2`, etc.).

## Architecture Overview

### Zig as Host
```
udynlink/udynlink.h          Zig host firmware
       |                           |
       v                           v
   C loader  <---------------->  @cImport of headers
   (udynlink.c)                  + Zig wrapper structs
                                 + Idiomatic call macros
                                 + Default externals impl
```

**Key design points:**
- Use `@cImport` for `udynlink.h`, `udynlink_externals.h`, `udynlink_call.h`.
- Re-implement `UDYNLINK_PREPARE_CALL` / `UDYNLINK_CALL` in Zig inline
  assembly (`asm volatile ("mov r9, %0" …)`).
- Provide a `Module` struct with methods: `load(image, mode)`, `unload()`,
  `resolve(comptime T, name)`, `call(comptime T, name, args)`.
- Provide a default `externals.zig` that wires `udynlink_external_malloc/free`
  to `std.heap.raw_c_allocator` (or a user-provided allocator) and
  `udynlink_external_vprintf` to `std.log`.

### Zig as Module
```
Zig source (.zig)
       |
       v
 zig build-obj -target thumb-freestanding-eabi -mcpu cortex_m4 -fPIE
       |
       v
   ELF object (.o)  ----->  existing mkmodule pipeline?  ---->  UDLM
       |                         (link + process)
       |                         OR
       +---> C-shim wrapper (if Zig PIC is incompatible)
```

**Critical open technical question:**
> Does Zig's LLVM backend for `thumb-freestanding-eabi` emit `R_ARM_GOT_BREL`
> relocations when compiled with `-fPIE`, matching GCC's
> `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative` model?
>
> Clang/LLVM does **not** support the GCC-specific ARM flags
> `-msingle-pic-base` or `-mno-pic-data-is-text-relative`. LLVM's ARM PIC
> codegen uses a different mechanism (GOT + possibly `R_ARM_GOT_PREL` or
> `R_ARM_BASE_PREL`). If the relocation types differ, the existing `mkmodule`
> pipeline will not recognize them.

## Decisions (user-confirmed)

1. ✅ **Zig compiler availability**: Provide `just setup-zig` — download and
   pin a specific Zig release (e.g. 0.14.0) into `tools/` or `tests/` for
   hermetic builds and reproducible CI.

2. ✅ **Idiomatic API depth**: **Level B (medium)** — Zig error unions
   (`!Module`), `resolveFunc(comptime T)` for type-safe handles, automatic
   `r9` save/restore in `call()`. Good balance of ergonomics and effort.

3. ✅ **Zig module fallback strategy**: If native Zig PIC is incompatible,
   use **C-shim workaround** — C exports (GCC PIC) call into a Zig static
   library (non-PIC). Documented clearly. Do **not** pursue extending the
   toolchain for new relocation types unless explicitly requested later.

4. ✅ **Test host firmware language**: **Full QEMU host tests** — build a
   parallel Zig host firmware test harness that runs under QEMU. This is
   higher complexity (Zig startup code, linker scripts) but provides full
   end-to-end validation.

## Spike Result: Branch B (C-Shim Workaround) — **COMMITTED**

**Verdict:** Zig's LLVM backend for `thumb-freestanding-eabi` is **incompatible** with udynlink's GCC-based `r9`/LOT model.

**Key findings from Task 0.1 (`zig-spike/zig-pic-spike.md`):**
- Zig emits `R_ARM_REL32` (PC-relative data access) and `R_ARM_THM_JUMP24`/`R_ARM_THM_CALL` (external function calls).
- No `R_ARM_GOT_BREL`, no `.got` sections, no `r9` usage.
- `mkmodule` would fail: it only recognizes `R_ARM_GOT_BREL`, `R_ARM_ABS32`, and `R_ARM_TARGET1`.
- **XIP mode is fundamentally broken**: PC-relative data assumes code and data are at a fixed offset. In XIP mode, code stays in flash while data is copied to RAM, breaking the offset.
- **External function calls cannot be resolved at load time**: Direct `bl`/`b.w` encodes a PC-relative offset to the target. The loader cannot patch this because the instruction encoding is limited to a small PC-relative range.
- An `llc -relocation-model=rwpi` workaround produces `R_ARM_SBREL32` and `r9`-based data access, but external calls still use `R_ARM_THM_JUMP24`, and this requires a two-step Zig → LLVM IR → `llc` toolchain not integrated with `mkmodule`.

**Decision:** Pursue **Branch B** — C-shim workaround. The C shim handles exports, imports, and data access via GCC's PIC model (`-fPIE -msingle-pic-base -mno-pic-data-is-text-relative`). The shim calls into Zig functions compiled as a static, non-PIC object. This preserves udynlink's architecture, XIP support, and host-call model.

## Execution Order & Parallelism

```
Phase 0: Spike ✅ COMPLETE — Branch B committed
    |
    +---> Phase 1: Zig Host (can start immediately)
    |         Task 1.1 (skeleton) — pure Zig, no deps.
    |         Task 1.2 (API) — needs @cImport of udynlink headers.
    |         Task 1.3 (compile-check) — needs Zig installed (already done by spike).
    |
    +---> Phase 2: Zig Module (C-shim workflow — Branch B)
    |         Task 2B.1 (C-shim + Zig static library workflow)
    |         Task 2B.2 (Extend toolchain for pre-compiled objects)
    |
    +---> Phase 3: Integration Tests
    |         Task 3.1 (Zig module QEMU test) — C host loads C-shim + Zig module.
    |         Task 3.2 (Zig host QEMU test) — Zig host loads C module.
    |
    +---> Phase 4: Docs & Polish
```

**Immediate-start tasks** (no dependency on spike):
- Task 1.1: Zig Host Package Skeleton — pure Zig code, can be written before Zig is even installed.
- Task 1.2: Idiomatic Zig Host API — design work, can be drafted in parallel with the spike.

## Atomic Task Breakdown

All tasks are independent unless marked with **[dep: X]**.

### Phase 0 — Spike (Resolves Open Technical Risk) — **START FIRST**

**Task 0.1: Verify Zig PIC Output on ARM Cortex-M**
- **Agent**: `shell` + `quick`
- **Goal**: Install Zig via `just setup-zig`, compile a minimal Zig file with
  `-target thumb-freestanding-eabi -mcpu cortex_m4 -fPIE` (and `-fPIC`), and
  inspect relocations with `llvm-readobj -r` or `readelf -r`.
- **Deliverable**: A report (`zig-pic-spike.md`) answering:
  1. What relocation types are emitted for global data references?
  2. Does the object contain a `.got` section?
  3. Are exported Zig functions (`export fn`) visible as `STT_FUNC` / `STB_GLOBAL`
     in the ELF symbol table (required for prologue generation)?
  4. Can `zig build-obj` output be linked with `arm-none-eabi-ld` using the
     existing `code_before_data.ld` script?
- **Time estimate**: 1–2 hours.
- **Escalation path**: If results are ambiguous, escalate to `deep` for
  analysis of LLVM ARM PIC codegen vs. GCC's `msingle-pic-base` model.
- **Blocking**: Yes — determines Branch A vs. B for Phase 2.

### Phase 1 — Zig as Host

**Task 1.1: Zig Host Package Skeleton**
- **Agent**: `agent`
- **Goal**: Create `zig/` directory with a `build.zig` that builds a static
  library or package. Add `@cImport` wrappers for the C headers.
- **Deliverable**:
  - `zig/build.zig` — package manifest, declares dependency on `udynlink/` C sources.
  - `zig/src/udynlink.zig` — `@cInclude("udynlink.h")`, `@cInclude("udynlink_call.h")`.
  - `zig/src/externals.zig` — default implementations of
    `udynlink_external_malloc`, `free`, `vprintf`, `resolve_symbol`.
- **[dep: 0.1]** only if we want to test-compile against the spike results,
  otherwise fully independent.

**Task 1.2: Idiomatic Zig Host API**
- **Agent**: `agent`
- **Goal**: Design and implement the ergonomic layer.
- **Deliverable**:
  - `zig/src/host.zig`:
    ```zig
    pub const Module = struct {
        raw: c.udynlink_module_t,
        pub fn load(image: []const u8, mode: LoadMode) !Module { ... }
        pub fn unload(self: *Module) void { ... }
        pub fn resolve(self: *const Module, comptime T: type, name: []const u8) !T { ... }
        pub fn call(self: *const Module, comptime T: type, name: []const u8, args: anytype) !T { ... }
    };
    ```
  - `zig/src/asm.zig` — Zig inline-assembly equivalents of
    `UDYNLINK_PREPARE_CALL` and `UDYNLINK_CALL` that work for both
    prologued and `--no-prologue` modules.
- **Review goal**: Compile against `udynlink.h` without errors on a mock
  `thumb-freestanding-eabi` target.

**Task 1.3: Zig Host Integration Test (Compile-Only)**
- **Agent**: `quick`
- **Goal**: Ensure the Zig host package compiles in CI.
- **Deliverable**:
  - `zig/tests/compile_check.zig` — a small Zig program that imports the
    package, creates a `Module`, and calls `resolve`/`call` with comptime types.
  - Update `.github/workflows/ci.yml` (or a new `zig-ci.yml`) to install Zig
    and run `zig build` for the package.

### Phase 2 — Zig as Module (Branch B: C-Shim Workaround) — **COMMITTED**

**Task 2B.1: C-Shim + Zig Static Library Workflow**
- **Agent**: `agent`
- **Goal**: Provide a working path where the UDLM entry points are C functions
  (compiled by GCC with correct PIC flags) that call into a Zig static library.
- **Deliverable**:
  - `zig/examples/c_shim_module/`:
    - `shim.c` — exports `hello`, `add`, etc., compiled with GCC/`-fPIE`.
    - `logic.zig` — actual implementation, compiled with `zig build-obj` as
      a *non-PIC* object and linked into the C shim.
    - `build.zig` — orchestrates the two-step build.
  - `docs/zig-module-guide.md` — documents the limitation and the C-shim
    workaround.

**Task 2B.2: Extend mkmodule for Pre-Compiled Object Files**
- **Agent**: `agent`
- **Goal**: Allow `mkmodule` to accept pre-compiled `.o` files alongside `.c` files.
- **Deliverable**:
  - Modify `scripts/mkmodule` to detect `.o` inputs and pass them through to
    the linker without compiling.
  - Ensure the linker command includes Zig objects alongside C objects.
  - Handle `compiler_rt` linking issues: when `-nostdlib` is used, Zig's
    runtime may need to be linked explicitly or avoided by disabling features.
  - This is the minimal toolchain change needed; the heavy lifting is done
    by the C shim compiled with GCC.
- **Note:** This is not a full "Zig module" toolchain — it's a "C module with
  Zig logic" toolchain. The user experience is: write Zig logic, write a thin
  C shim for exports/imports, run `mkmodule` with both `.c` and `.o` inputs.

### Phase 3 — Integration Testing

**Task 3.1: Zig Module QEMU Test**
- **Agent**: `agent`
- **Goal**: Add a real integration test where a C host loads a Zig module.
- **Deliverable**:
  - `tests/test-zig-module/`:
    - `mod_hello.zig` — exports `hello(arg: i32) void` that calls host `printf`.
    - `test_qemu.c` — standard C host that loads the Zig module and calls `hello`.
    - `test_data.py` — test metadata.
  - The test driver (`tests/test_driver.py`) needs to know how to invoke the
    Zig module build step. We may need to add a `pre_build` hook in
    `test_data.py` or extend the driver.

**Task 3.2: Zig Host QEMU Test (Optional)**
- **Agent**: `expert`
- **Goal**: Run a Zig host firmware under QEMU that loads a C module.
- **Deliverable**:
  - `tests/zig_host/`:
    - `main.zig` — Zig firmware: sets up UART, implements externals, loads
      `mod_hello_module_data`, calls `hello()`.
    - `build.zig` — builds an ELF linked with `udynlink.c` for the target.
    - A runner script that invokes QEMU.
  - **Complexity**: High. Zig's freestanding startup (vector table, linker
    script) must match the existing platform definitions in
    `tests/platforms/*/`. May need a `gatz`-style translation or a custom
    linker script.

### Phase 4 — Documentation & Polish

**Task 4.1: Update docs/
- **Agent**: `agent`
- **Goal**: Add Zig-specific sections to the user-facing docs.
- **Deliverable**:
  - `docs/zig-host.md` — integrating the Zig host package, implementing
    callbacks, calling module functions.
  - `docs/zig-module.md` — writing loadable Zig modules, build commands,
    FFI patterns.
  - Update `docs/integrating-as-host.md` and `docs/writing-modules.md` with
    cross-references.

**Task 4.2: Update AGENTS.md**
- **Agent**: `quick`
- **Goal**: Record new build commands and constraints.
- **Deliverable**: Add `just build-zig`, `just test-zig-module`, etc. to the
  commands table and note Zig toolchain requirement.

## Review Checkpoints

| Checkpoint | When | Review Goal |
|------------|------|-------------|
| **Spike complete** ✅ | After Task 0.1 | **Branch B committed.** Zig PIC is incompatible with udynlink's `r9`/LOT model. |
| **Host API ready** | After Task 1.2 | Review `host.zig` API surface. Is it idiomatic enough? |
| **Module builds** | After Task 2A.1 or 2B.1 | Verify a Zig module can be built and loaded manually. |
| **First green test** | After Task 3.1 | `just test-zig-module` passes on at least one QEMU platform. |
| **Docs complete** | After Task 4.1 | User can follow the guide without prior udynlink knowledge. |

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| Zig PIC output is incompatible | ~~Blocks native Zig modules~~ | **Resolved:** Using C-shim workflow (Branch B). |
| `compiler_rt` linking fails in `-nostdlib` | Blocks Zig modules using builtins | Disable builtins or manually link `libcompiler_rt.a`. |
| Zig inline asm for `r9` save/restore is fragile | Runtime crashes on host calls | Test on multiple QEMU platforms; provide `--no-prologue` path. |
| Zig host startup code diverges from C platforms | QEMU tests fail | Use existing platform linker scripts; minimize Zig-specific startup. |
| Zig version churn | API breakage | Pin to 0.14.0 (already done in spike). |

## Next Steps

1. ✅ Spike complete — Branch B committed.
2. **Start Phase 1** — Tasks 1.1 and 1.2 can begin immediately (Zig host skeleton + API).
3. **Start Phase 2** — Task 2B.1 (C-shim workflow) can begin once the host API is stable.
4. **Run Phase 3** — Integration tests after host and module workflows are ready.
5. **Finish Phase 4** — Documentation and polish.
