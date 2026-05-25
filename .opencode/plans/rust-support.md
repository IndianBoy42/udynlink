# Rust Support for udynlink — Planning Document

**Status:** Draft — Awaiting user review and clarifications  
**Last updated:** 2026-05-25

---

## 1. Goal

Add comprehensive Rust support to udynlink in **two independent directions**:

1. **Rust as Host** — A Rust crate that can load and call udynlink modules (currently produced from C/C++ via `mkmodule`).
2. **Rust as Module** — A build pipeline and crate support that lets developers write `no_std` Rust crates, compile them, and produce udynlink-compatible loadable module binaries.

---

## 2. Current Architecture Summary

*(For context for future implementation agents — see `codemap.md` and `AGENTS.md` for full details.)*

### 2.1 udynlink Module Format
- Binary modules start with signature `UDLM`, followed by header, relocation table, symbol table, `.text`, `.data`.
- The **LOT (Linker Offset Table)** lives at the start of the module's RAM region. `r9` points to the LOT base.
- Exported functions get an **assembly prologue** (from `asm_template.tmpl`) that pushes `r9`/`lr`, loads `r9` from `*(uint32_t*)0x20000000`, calls the real function, then pops `r9`/`pc`.
- Data access in C modules uses **GOT-based indirection**: compiler generates `R_ARM_GOT_BREL` relocations → mkmodule patches them to point to LOT entries → at runtime, LOT entries hold absolute addresses.

### 2.2 Build Pipeline (`scripts/mkmodule`)
1. **Compile** C/C++ with `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative`
2. **Wrap** exported symbols via `objcopy --redefine-sym` + generate assembly prologues
3. **Link** with `code_before_data.ld` (origin 0, `.text` → `.data` → `.bss`)
4. **Process** ELF: extract sections, classify symbols, parse relocations, build LOT, patch code, emit binary

### 2.3 Loader API
- `udynlink_load_module()` — loads module, applies relocations, resolves externs
- `udynlink_lookup_symbol()` / `udynlink_get_symbol_value()` — find exported functions/data
- Host must implement `udynlink_externals.h` (malloc, free, vprintf, resolve_symbol, is_pointer_in_ram)
- **Critical**: Before calling any module function, host must write `ram_base` to `*(uint32_t*)0x20000000`

### 2.4 Test Infrastructure
- `tests/test_driver.py` orchestrates: compile module → build QEMU host firmware → run in `qemu-system-gnuarmeclipse` → validate output
- Each test runs 6 times: `-O0`/`-Os` × 3 load modes (`COPY_ALL`, `COPY_CODE`, `XIP`)
- Host firmware in `tests/qemu_host/` is an Eclipse-generated STM32F429 project

---

## 3. Rust Embedded Ecosystem Context

### 3.1 Relevant Rust Targets
- `thumbv7em-none-eabihf` — Cortex-M4F (matches current udynlink target, already installed)
- Rust `no_std` + `alloc` is the standard approach for embedded

### 3.2 Position-Independent Code in Rust
- Rustc supports `-C relocation-model=ropi`, `rwpi`, `ropi-rwpi` on ARM targets
- **RWPI** uses `r9` as the **static base register** — data accessed as `[r9, #offset]` directly
- **ROPI** makes code and `.rodata` PC-relative
- **Known bugs**: `rwpi` / `ropi-rwpi` have codegen issues in debug builds (rust-lang/rust#95871). Workaround: `-C codegen-units=1`
- **Vtable issue**: Dynamic dispatch (trait objects) generates non-position-independent vtables in `.data` (rust-lang/rust#54431)

### 3.3 Critical Architectural Difference
| Aspect | C (GCC) + udynlink | Rust (LLVM) + RWPI |
|--------|-------------------|-------------------|
| Data access model | GOT-based indirection via LOT | Direct `r9`-relative offsets |
| Relocations | `R_ARM_GOT_BREL` | SB-relative (different relocation types) |
| Runtime setup | `r9` → LOT base, LOT entries patched | `r9` → start of `.data`, offsets are static |
| Prologue needs | Load `r9` from `0x20000000` | Same, but `r9` points directly to data, not LOT |

**This means Rust RWPI modules would generate a different relocation pattern than the current C udynlink loader expects.**

---

## 4. Proposed Architecture

### 4.1 Workstream A: Rust Host (`udynlink-rs` crate)
**Goal:** Load existing C/C++ udynlink modules from a Rust program.

**Approach:**
- Provide a `udynlink-rs` crate with two layers:
  1. **Raw FFI layer** (`udynlink-sys`) — binds to existing C library (`udynlink.c` + `udynlink.h`)
  2. **Safe API layer** — idiomatic Rust wrappers around loading, symbol lookup, and calling
- Provide a trait-based `UdynlinkExternals` so Rust hosts implement the 5 required functions
- Automate the `0x20000000` LOT-base write before calls (via a wrapper struct or call guard)
- Support `no_std` usage (optional `alloc` feature)

**Why this is independent:** It only consumes the existing C udynlink library and module format. No changes to the loader or build pipeline needed.

### 4.2 Workstream B: Rust Module Support (`udynlink-module` crate + build tool)
**Goal:** Compile Rust crates into udynlink-compatible binary modules.

**Three possible approaches, in order of feasibility vs. ergonomics:**

#### Approach B1: "C-compatible binary" (Most compatible with existing loader)
- Compile Rust with `-C relocation-model=static` (or `ropi` for code)
- Do **not** use Rust's RWPI
- Manually ensure data accesses go through a LOT-like mechanism (hard, unergonomic)
- **Verdict:** Not practical for real Rust code. Rejected.

#### Approach B2: "RWPI-native with loader extension" (Most ergonomic for Rust)
- Compile Rust with `-C relocation-model=ropi-rwpi -C codegen-units=1`
- Design a **new module format variant** (or loader mode) for RWPI-style relocations
- Exported functions still need assembly prologues to set `r9`
- The loader (`udynlink.c`) is extended to handle SB-relative relocations
- Module RAM layout changes: no LOT needed, just `.data`/`.bss` directly
- **Verdict:** Best long-term, but requires C loader modifications and testing.

#### Approach B3: "Post-processed hybrid" (Best pragmatic balance)
- Compile Rust with `-C relocation-model=static` + `-C link-arg=-T...` + custom linker script
- Build a Rust-side "module runtime" crate that provides:
  - A `#[udynlink_export]` proc-macro that wraps functions with prologues (inline asm)
  - A static LOT-like table generated at compile time for exported data
  - A `#[no_mangle]` + custom section discipline for the symbol table
- A build tool (`cargo-udynlink` or post-build script) parses the Rust ELF, extracts sections/symbols/relocations, and emits the standard `UDLM` binary
- The tool handles converting Rust's absolute/relative relocations into the existing udynlink relocation format
- **Verdict:** Most practical for immediate results. Can evolve into B2 later.

**Recommended:** Start with **Approach B3** for pragmatic progress, with architecture designed to migrate toward **B2** as the loader matures.

### 4.3 Workstream C: Test Infrastructure
- Add Rust-based tests to the existing QEMU harness
- Create a Rust test host firmware (alternative to `tests/qemu_host/`)
- Or: add a Rust module to an existing C test and verify cross-language loading

---

## 5. Task Breakdown

### Phase 1: Foundation & Rust Host (Independent)
> **Can be done in parallel. No dependencies on Phase 2.**

| # | Task | Agent | Outcome |
|---|------|-------|---------|
| 1.1 | Create `udynlink-sys` FFI crate | `agent` | `udynlink-sys/` with `build.rs` linking to `udynlink/udynlink.c`, raw `extern "C"` bindings |
| 1.2 | Create `udynlink-rs` safe API crate | `agent` | `udynlink-rs/` with `Module`, `Symbol`, safe load/unload/lookup/call wrappers |
| 1.3 | Implement `UdynlinkExternals` trait + example host | `agent` | Trait definition + example `no_std` / `std` implementations |
| 1.4 | Add Rust host test to QEMU harness | `expert` | A Rust program that loads a C module and calls it, running in QEMU |
| 1.5 | Documentation & examples | `quick` | README, examples for host usage |

### Phase 2: Rust Module Support (Build Pipeline)
> **Depends on Phase 1 for integration testing, but build tool can be prototyped independently.**

| # | Task | Agent | Outcome |
|---|------|-------|---------|
| 2.1 | **Spike:** Compile a minimal `no_std` Rust binary for thumbv7em and inspect relocations | `search` + `shell` | Report: what relocation types does Rust generate with `ropi`, `rwpi`, `ropi-rwpi`, `static`? |
| 2.2 | Design `udynlink-module` crate (runtime + proc-macros) | `deep` | Crate spec: `#[udynlink_export]`, panic handler, minimal alloc, LOT discipline |
| 2.3 | Implement `udynlink-module` runtime crate | `agent` | `udynlink-module/` crate: proc-macro, panic handler, inline asm prologues |
| 2.4 | Design module build tool (`cargo-udynlink` or post-build script) | `deep` | Tool architecture: cargo subcommand or build.rs helper that produces `.bin` |
| 2.5 | Implement build tool (ELF → UDLM post-processing) | `agent` | Working tool that takes Rust ELF and emits `UDLM` binary |
| 2.6 | Write a Rust module test and validate against C host | `expert` | A Rust module loaded by the existing C test harness, runs in QEMU |
| 2.7 | Cross-test: Rust host loads Rust module | `expert` | End-to-end: Rust program loads Rust module, both running in QEMU |

### Phase 3: Polish & Integration

| # | Task | Agent | Outcome |
|---|------|-------|---------|
| 3.1 | CI integration (GitHub Actions) | `quick` | Extend `.github/workflows/ci.yml` with Rust toolchain install and Rust tests |
| 3.2 | `cargo` workspace organization | `agent` | Top-level `Cargo.toml` workspace including all crates |
| 3.3 | Documentation for module authors | `quick` | Guide: writing a `no_std` udynlink module in Rust |
| 3.4 | Cargo publish preparation | `quick` | `Cargo.toml` metadata, versioning, crate naming |

---

## 6. Key Decision Points & Open Questions

### Decision 1: Which direction first?
**Rust Host** is lower risk and fully independent. **Rust Module** is where the real novelty is but faces unknowns around relocation compatibility. Should we start with both in parallel, or sequence them?

### Decision 2: Rust module relocation model
- **Option A:** Use `ropi-rwpi` natively → requires extending the C loader with new relocation handling. Cleanest for Rust, but invasive.
- **Option B:** Use `static` + custom proc-macro/runtime to mimic the existing C/GOT model. More compatible with existing loader, but less ergonomic and may not support all Rust features.

### Decision 3: Exported function prologues in Rust
- **Option A:** Proc-macro `#[udynlink_export]` wraps the function body with inline assembly. Most idiomatic for Rust authors.
- **Option B:** Post-process the compiled ELF with objcopy + assembly (like `mkmodule` does for C). Matches existing toolchain but requires demangling Rust symbol names.

### Decision 4: Build tool style
- **Option A:** `cargo-udynlink` subcommand (like `cargo build` but for modules)
- **Option B:** A library crate + `build.rs` integration in the user's module crate
- **Option C:** Standalone script/tool (like `mkmodule` but Rust-aware)

### Decision 5: Safety vs. ergonomics for host API
- **Option A:** `unsafe` thin wrapper around C FFI — minimal abstraction, maximum control
- **Option B:** Fully safe API with `Module`/`Symbol` structs that manage lifetime and automatically set the LOT base — requires designing safe invariants

### Decision 6: Target scope
- Current udynlink hardcodes Cortex-M4 (`-mcpu=cortex-m4`).
- Should Rust support target other Cortex-M variants (M0+, M3, M7)?
- Should we make the target configurable from the start?

### Decision 7: Hardcoded `0x20000000`
- The LOT base address is STM32F429-specific.
- Should the Rust host API provide a way to configure this per-target?
- Should module prologues reference a configurable symbol instead of the hardcoded literal?

---

## 7. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Rust `rwpi`/`ropi-rwpi` codegen bugs prevent working modules | High | High | Use `-C codegen-units=1`; restrict to release builds; test extensively |
| Rust generates relocation types the loader can't handle | Medium | High | Phase 2.1 spike discovers this early; may need loader extension |
| Vtables / trait objects non-PIC in `.data` | Medium | Medium | Document restriction: no `dyn Trait` in modules; verify in testing |
| Rust panic handling in modules | Medium | Medium | Provide custom panic handler in `udynlink-module`; document abort-only |
| Symbol name mangling complicates post-processing | Medium | Medium | Use `#[no_mangle]` on exports; proc-macro can enforce this |
| QEMU test harness limitations (Eclipse makefiles) | Low | Medium | May need to add Rust compilation to Eclipse makefile or create parallel test harness |

---

## 8. Files & Directories to Create

```
udynlink/
├── udynlink-sys/          # FFI bindings (Phase 1.1)
├── udynlink-rs/           # Safe Rust API (Phase 1.2)
├── udynlink-module/       # Rust module runtime + proc-macros (Phase 2.3)
├── cargo-udynlink/        # Cargo subcommand or build helper (Phase 2.4-2.5)
├── Cargo.toml             # Workspace root (Phase 3.2)
└── rust_tests/            # Rust-specific tests and examples
```

---

## 9. Next Steps

1. **User review** of this document and answers to the open questions in Section 6.
2. Refine the plan based on decisions.
3. Create sub-planning documents for individual phases.
4. Begin execution with Phase 1 (Rust Host) while Phase 2 (Module Support) planning continues.

---

*End of planning document.*
