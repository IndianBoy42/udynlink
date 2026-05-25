# Rust Support for udynlink — Master Plan

**Status:** Awaiting user review of sub-plans  
**Last updated:** 2026-05-26  
**Question 1 answer:** Start with Rust Module (post-processing)  
**Question 2 answer:** Post-process Rust ELF into existing UDLM format (compatible with current loader)  
**Question 3 answer:** Proc-macro `#[udynlink_export]` with inline asm prologues  
**Question 4 answer:** Standalone tool first, then cargo integration  
**Question 5 answer:** Both layers (unsafe base + safe wrapper)  
**Question 6 answer:** Any target via target database  
**Question 6,7 answer:** Configurable LOT base (UDYNLINK_LOT_BASE_ADDR already implemented in C)

---

## 1. Overview

This plan adds Rust support to udynlink in two independent directions:

1. **Rust Host** — Load and call udynlink modules (produced from C/C++) from a Rust program.
2. **Rust Module** — Compile `no_std` Rust crates into udynlink-compatible binary modules.

Each direction is independent enough to be developed, tested, and merged separately. A Rust host can load C modules immediately. A Rust module can be loaded by a C or Rust host once the binary is produced.

---

## 2. Architecture Decisions

| Decision | Chosen Approach | Rationale |
|----------|----------------|-----------|
| **Direction priority** | Rust Module first, then Rust Host | User explicitly asked to start with post-processing module support |
| **Module relocation model** | Post-process Rust ELF to existing UDLM format | No C loader modifications needed; leverages mature loader; avoids Rust PIC/RWPI bugs |
| **Export mechanism** | Proc-macro `#[udynlink_export]` with inline asm prologues | Idiomatic Rust; avoids objcopy symbol mangling issues; same runtime semantics as C wrappers |
| **Build tool style** | Standalone CLI (`rust2udynlink`) first, then `cargo-udynlink` subcommand | Immediate CLI for experimentation, then cargo integration for real workflows |
| **Host API safety** | Both: `udynlink-sys` (raw unsafe FFI) + `udynlink-rs` (safe wrappers) | Users choose their abstraction level |
| **Target support** | All 9 targets via `scripts/targets.py` database | Match existing C toolchain; arch-tag validation in loader ensures safety |
| **LOT base address** | Respect `UDYNLINK_LOT_BASE_ADDR` compile-time macro | Already configurable in C; Rust prologues reference configurable symbol |

---

## 3. Workstream A: Rust Module (`rust-module`)

**Goal:** Compile `no_std` Rust code into a `.bin` file that `udynlink_load_module()` can load.

**Key Insight:** The C toolchain produces a UDLM binary by:
1. Compiling C with GCC flags that generate specific relocations (`-fPIE -msingle-pic-base -mno-pic-data-is-text-relative`)
2. Wrapping exported functions with assembly prologues (save/restore r9, load LOT base)
3. Post-processing the ELF: extracting sections, classifying symbols, building a LOT, patching `R_ARM_GOT_BREL` relocations, emitting UDLM

For Rust, we replicate this pipeline:
1. **Compile** Rust with flags and custom linker script to generate relocatable ELF
2. **Export** functions via `#[udynlink_export]` proc-macro (injects inline asm prologue + `#[no_mangle]`)
3. **Post-process** the Rust ELF with `rust2udynlink` tool, converting Rust relocations into udynlink's LOT format
4. **Emit** standard UDLM binary

The critical uncertainty is: **what relocation types does Rust generate, and can they be mapped to udynlink's model?** This is answered in the sub-plan's Phase 0 (investigation spike).

**Sub-plan:** [`.opencode/plans/rust-module.md`](./rust-module.md)

---

## 4. Workstream B: Rust Host (`rust-host`)

**Goal:** Load udynlink modules and call their exports from Rust.

**Architecture:**
- **Layer 1** (`udynlink-sys`): Raw `extern "C"` FFI bindings to `udynlink.h` and `udynlink_externals.h`.
- **Layer 2** (`udynlink-rs`): Safe wrappers — `Module` struct with RAII unload, `Symbol<T>` for typed function pointers, automatic LOT-base write via a call guard.
- **Layer 3** (integration): Trait-based `UdynlinkExternals` so the host implements malloc/free/resolve_symbol in Rust.

This workstream is **independent** of the module workstream. A Rust host can load existing C modules immediately.

**Sub-plan:** [`.opencode/plans/rust-host.md`](./rust-host.md)

---

## 5. Test Integration

Both workstreams must integrate with the existing QEMU test harness:
- `tests/test_driver.py` orchestrates compilation → build host → run QEMU → validate
- Add `test-rust-module` and `test-rust-host` test cases
- Each test runs with `-O0` and `-Os`, across all 3 load modes
- Platforms: at minimum MPS2-AN386 (Cortex-M4), others as needed

---

## 6. Directory Structure

```
udynlink/
├── udynlink/                    # Existing C core library
├── scripts/                     # Existing Python toolchain
├── tests/                       # Existing test harness
├── Cargo.toml                   # NEW: Workspace root
├── crates/
│   ├── udynlink-sys/            # NEW: FFI bindings
│   ├── udynlink-rs/             # NEW: Safe host API
│   ├── udynlink-module/         # NEW: Module runtime + proc-macros
│   └── cargo-udynlink/          # NEW: Cargo subcommand / CLI
└── .opencode/plans/
    ├── rust-support.md          # This file
    ├── rust-module.md           # Sub-plan: Rust module support
    └── rust-host.md             # Sub-plan: Rust host support
```

---

## 7. Execution Order

1. **Phase 0** (both paths): Read and validate this plan with user → refine → approve
2. **Phase 1** (Rust Module): Investigation spike → standalone tool → proc-macro → first Rust module test
3. **Phase 2** (Rust Host): FFI crate → safe wrapper → Rust host test loading C module
4. **Phase 3** (Integration): Cross-test Rust host loading Rust module, cargo-udynlink, CI, docs

---

*End of master plan. See sub-plans for detailed task breakdown and difficulty analysis.*
