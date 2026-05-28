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
| **Module relocation model** | `static` with MOVW/MOVT loader extension | Investigation shows no Rust model produces `R_ARM_GOT_BREL`; `static` model works reliably with existing `core` lib |
| **XIP mode** | Not supported for Rust modules | Rust `static` model encodes absolute addresses in instructions; XIP keeps code in flash (read-only), so instructions cannot be patched |
| **Export mechanism** | Proc-macro `#[udynlink_export]` with generated assembly prologues | Idiomatic Rust; avoids objcopy symbol mangling; post-processor generates wrappers like C toolchain |
| **Build tool style** | Standalone CLI (`rust2udynlink`) first, then `cargo-udynlink` subcommand | Immediate CLI for experimentation, then cargo integration for real workflows |
| **Host API safety** | Both: `udynlink-sys` (raw unsafe FFI) + `udynlink-rs` (safe wrappers) | Users choose their abstraction level |
| **Target support** | All 9 targets via `scripts/targets.py` database | Match existing C toolchain; arch-tag validation in loader ensures safety |
| **LOT base address** | Respect `UDYNLINK_LOT_BASE_ADDR` compile-time macro | Already configurable in C; Rust prologues reference configurable symbol |

---

## 3. Workstream A: Rust Module (`rust-module`)

**Goal:** Compile `no_std` Rust code into a `.bin` file that `udynlink_load_module()` can load.

**Key Insight (from investigation):**
- **No Rust relocation model produces `R_ARM_GOT_BREL`** (the relocation type the existing loader is built around).
- The `core` library is **always compiled with `static` relocation**, so even `-C relocation-model=pic` produces thousands of absolute `MOVW`/`MOVT` relocations.
- **Recommended model:** `-C relocation-model=static` — simplest, most reliable, and matches `core`.
- **Required change:** Extend the C loader to patch `R_ARM_THM_MOVW_ABS_NC` and `R_ARM_THM_MOVT_ABS` relocations (decode/encode Thumb-2 immediate fields).
- The existing `R_ARM_ABS32` and `R_ARM_TARGET1` data relocation handling in the loader already works for Rust.

The C toolchain pipeline (for comparison):
1. Compile C with GCC flags that generate `R_ARM_GOT_BREL` + `R_ARM_ABS32` relocations
2. Wrap exported functions with assembly prologues
3. Post-process ELF: build LOT, patch code, emit UDLM

The Rust pipeline (new):
1. Compile Rust with `-C relocation-model=static -C panic=abort -C codegen-units=1`
2. Use custom linker script (code at 0x0, .data after, .bss after)
3. Mark exports with `#[udynlink_export]` proc-macro (metadata in custom ELF section)
4. Post-process with Python tool: handle MOVW/MOVT relocations, generate assembly prologues, emit UDLM
5. Extended loader patches MOVW/MOVT instructions at load time

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
- **Rust modules:** Test with `-O0` and `-Os`, but only `COPY_ALL` and `COPY_CODE` modes (XIP returns expected error)
- **Rust host:** Can load existing C modules (all 3 modes, including XIP)
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
2. **Phase 1** (Rust Module): Extend C loader for MOVW/MOVT → Python post-processor → proc-macro → first Rust module test
3. **Phase 2** (Rust Host): FFI crate → safe wrapper → Rust host test loading C module
4. **Phase 3** (Integration): Cross-test Rust host loading Rust module, cargo-udynlink, CI, docs

---

*End of master plan. See sub-plans for detailed task breakdown and difficulty analysis.*
