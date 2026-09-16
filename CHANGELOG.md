# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Multi-region memory placement (code + data sections)** — a module built with `mkmodule --section NAME[:align=N][:flags=...]` (repeatable; hint tokens `NOCACHE`, `DMA`, `SHARED`, host-reserved `host0`..`host7`) tags named code/data sections that the host places in dedicated memory regions (DTCM, DMA-capable SRAM, CCM, ...). The image gains a section table (header `reserved` field is now `flags`: bit 0 `UDYNLINK_HDR_FLAG_SECTIONS`, bits 7:1 the section count 1..63 — the table itself is count-less entries, so image size is computable from the header alone), payloads are stored in ascending VA order (BSS skipped), and section names share the symbol string pool. Loader ABI 3.0 → 3.1: sectioned images must declare `udynlink_version >= 3.1` (an old 3.0 loader rejects them with `VERSION_MISMATCH`); untagged modules keep version 3.0 and byte-identical images, layouts, and the exact 3.0 loader path. Symbol/relocation encodings are unchanged; resolution becomes VA-map based for sectioned images (`runtime(va) = sec_base[idx] + (va - sec[idx].va)`), with the two base-additive reloc forms collapsing to `*p = runtime(*p)` and untagged modules keeping the existing code/data-base path verbatim.
- **`udynlink_module_image_t`** — non-contiguous module image descriptor with per-section pointers (`p_header`, `p_relocations`, `p_symtab`, `p_code`, `p_data`). Decouples the relocation engine from source layout so users can load from decompressed, encrypted, or scattered buffers without copying everything into one contiguous blob first.
- **Section query/placement API** — `udynlink_get_section_count()`, `udynlink_get_section_info()`, `udynlink_get_section_base()` (untagged modules report the three implicit main sections with today's exact bases), `udynlink_section_info_t`, `udynlink_section_move_t`, and `udynlink_relocate_module_sections()` (moves host-copied tagged sections and re-applies the relocations that reference them with per-section deltas). New error codes appended after index 14: `UDYNLINK_ERR_LOAD_SECTION_UNRESOLVED` (15, allocator returned `NULL` for a section/main block), `UDYNLINK_ERR_LOAD_SECTION_UNALIGNED` (16, returned base below the declared alignment), and `UDYNLINK_ERR_LOAD_BAD_SECTION_TABLE` (17, malformed/inconsistent section table: unsorted or overlapping VAs, bad size/align/class, name offset outside the string pool, caps exceeded). `udynlink_get_image_size()` now counts `code_offset + Σ(CODE|DATA section sizes)`, so caller-side buffer-size gates cover sectioned images.
- **`udynlink_load_module_image()`** — high-level API that loads from a `udynlink_module_image_t`, copying sections into RAM according to the load mode and then applying relocations through the shared canonical path.
- **`udynlink_image_from_memory()` / `udynlink_image_from_module()`** — builders that populate an image descriptor from a contiguous UDLM buffer or from an already-loaded module handle.
- **Low-level loading primitives** — `udynlink_validate_header()`, `udynlink_compute_ram_size()`, `udynlink_get_image_metadata_size()`, `udynlink_image_get_module_name()`, and `udynlink_load_apply_relocations()`. These let advanced users implement custom loading pipelines (e.g., read header from SD card, validate, allocate RAM, copy sections chunk by chunk, then apply relocations).
- **`user_ctx` field on `udynlink_module_t`** — an opaque `void *` pointer that the loader never touches, provided for the host to associate arbitrary state (filesystem path, language runtime handle, reference counter, etc.) with a module handle.
- **`proto2module --extra-source` / `--extra-public`** — compile an extra C source (e.g. an api-compat sha embed) into the generated module and keep chosen symbol names public past `--strip-non-public-syms`, so host-side gates that look symbols up by name keep working on generated pb modules. Both repeatable.
- **mkmodule argument hardening** — unrecognized dash-prefixed arguments are rejected with an error naming them, instead of being handed to the compiler as input files (which surfaced as a confusing gcc "unrecognized command-line option" failure). One leading `--` separator is consumed; leading `-D` definitions keep working.
- **`udynlink_get_image_size_bounded()`** — total image size (including tagged-section payloads) computed without reading past a caller-supplied `avail`. The plain `udynlink_get_image_size()` dereferences only the header, so for a sectioned image it is a lower bound: a section table declaring a 256 KiB tagged payload in a 508-byte file passed a gate built on it and made the loader `memcpy` those bytes out of the input buffer (reproduced under ASan). The fuzz/sanitizer harness now gates on the bounded variant, and `just fuzz-seeds` plants a truncated sectioned seed so that case stays covered.
- **`udynlink_section_info_t.sec_class`** — the section class field is `sec_class`, not `class`: `class` is a C++ keyword, and a C++ host (or `udynlink.hpp`) including `udynlink.h` failed to compile.
- **mkmodule placement diagnostics** — building a module that tags a symbol into a section the image cannot carry (e.g. `__attribute__((section(".fastdata")))`) now fails with an error naming the offending section and its symbols, instead of a Python `KeyError`. Modules whose data needs more than 4-byte alignment (`__attribute__((aligned(N)))` with `N > 4`) emit a build-time warning naming the alignment and representative symbols, since the loader guarantees only 4-byte alignment for module data.
- **`UDYNLINK_ERR_LOAD_RAM_UNALIGNED`** — new loader error code (appended last as index 14; existing indices unchanged, public ABI preserved). `udynlink_load_module_image()` now validates that the module RAM base is word (4-byte) aligned on both establishment paths: a caller-supplied `load_addr` is rejected after the `load_size` check, and a misaligned `udynlink_external_malloc()` result is rejected (and freed) after the out-of-memory check, so a bad base fails the load instead of corrupting every 32-bit access into the block.

### Changed

- **`.text`→`.data` alignment gaps are packed into the module image** — a strictly aligned data member raises the packed `.data` section's `sh_addralign`, so the linker starts it after an alignment gap; `mkmodule` used to reject such modules ("Section '.data' doesn't begin after section '.text'"). It now pads the gap into the packed `.data` (mirroring the `.data`→`.bss` gap handling) so data symbol values and ABS32 relocations resolve inside the RAM arena; combined with the new alignment warning, aligned builds succeed while making the runtime 4-byte-alignment contract visible. Images without a gap (all existing modules) are byte-identical.
- **Alignment contract documented** — the API reference, host-integration guide, and public headers now state precisely what alignment the loader provides: word (4-byte) only, with the module data area starting at `p_ram + num_lot * 4`; a misaligned `load_addr` or `udynlink_external_malloc()` result is rejected with `UDYNLINK_ERR_LOAD_RAM_UNALIGNED`; and `__attribute__((aligned(N)))` for N > 4 is not honored at runtime (measured: a 32-byte-aligned object landed at `0x20000028` under a 32-byte-aligned RAM base). `udynlink_external_is_pointer_in_ram` docs now state the loader never calls it (`UDYNLINK_ERR_LOAD_XIP_UNSUPPORTED` is never returned), and the loader fuzz/san harness gained a deterministic misaligned-RAM oracle.

### Fixed

- **`udynlink_cpp_init` r9 clobber under sibling-call optimization** — At `-O2`/`-O3`/`-Os`/`-Oz` and under `-flto`, GCC turned the `__init_array` call into a tail call. The function's epilogue (`ldmia {…, r9, lr}`) restored the caller's `r9` *after* the inline-asm `UDYNLINK_PREPARE_CALL` set it to the LOT base, so the module's constructor runner was entered with the wrong `r9` and corrupted its data accesses. Prologue-wrapped modules masked the bug (their prologue re-sets `r9`), but C++ modules built with `--no-prologue` crashed. `udynlink_cpp_init` now saves/restores `r9` around the call, mirroring `UDYNLINK_CALL_VOID` — the restore clobber also acts as a hard barrier that defeats sibling-call optimization. Hosts that build `libudynlink` at `-Os` plus `-flto` should rebuild.
- **Host (x86) build of `udynlink.c` failed to link** — `udynlink_cpp_init`'s r9 save/restore asm used ARM assembly syntax, which x86 GAS assembles as a symbol reference (`R_X86_64_32S` against undefined `r9`), breaking every native build (`just test-san`, the fuzz harnesses). The asm is now compiled only on `__arm__`/`__thumb__` targets; other builds skip it (they never execute ARM code). ARM codegen is unchanged.
- **`just test-mps2` aborted on leftover output directories** — a `tests/test-*/` directory with no `test_data.py` and no sources (output of a test whose sources were deleted) made the driver build an empty module list, and `mkmodule`'s "Empty file/macro list" failed the whole run with no hint about the cause. The driver now skips such directories with an explicit message. Symptom seen locally as `test-cpp-empty-init-array` failing to build.
- **`stm32f429_discovery` placement-pool section moved to RAM** — `test-sections` failed on the legacy xPack QEMU model with `Bad ram pointer` when the host pool was linked into CCM RAM at `0x10000000`: the model does not provide usable memory there (the identical test passes with the pool in RAM, and section data as well as code were affected). Model limitation, not a loader bug; the second-region assertion is made on `mps2_an386`, whose BRAM the mainline model does back.

## [0.2.0] - 2026-05-27

### Changed

- **ABI v3.0** — Removed built-in dependency system (`--depends`, dep tracking, 3-tier resolution) and `UDYNLINK_LOT_BASE_ADDR`. Host now manages `r9` directly via `UDYNLINK_PREPARE_CALL()`. Prologue templates simplified to save/restore caller `r9`. Symbol resolution is single-tier via `udynlink_external_resolve_symbol()` only. Module header back to 32 bytes; loader ABI version is `3.0` (`0x0300`).

## [0.1.0] - 2026-05-26

### Added

- **Non-contiguous image loading** — load modules from SD card, SPI flash, or any non-memory-mapped source by assembling a `udynlink_module_image_t` from scattered buffers. Replaces the old `udynlink_io_t` streaming API with a more flexible descriptor-based approach that shares the same canonical relocation path as memory-backed loads.
- **Hash-based O(1) symbol resolution** — optional GNU hash table for host firmware symbol lookup. Adds `udynlink_hash.h`/`udynlink_hash.c` and the `scripts/mkhostsyms` tool that generates a const hash table from a host ELF.
- **Module dependency tracking** — declare dependencies at build time with `mkmodule --depends mod_a,mod_b`. The loader enforces that all declared dependencies are already loaded. Three-tier symbol resolution: critical host symbols → dependency modules → fallback host symbols. Safe unload via `dep_refcount` prevents unloading a module that has active dependents.
- **ABI versioning and architecture tag validation** — module headers include `mod_version`, `udynlink_version`, and `arch_tag`. The loader validates compatibility at load time and rejects modules compiled for a mismatched core family, FPU, or float ABI.
- **Compile-time target database** (`scripts/targets.py`) with per-target assembly prologue templates supporting Cortex-M0, M0+, M3, M4, M4F, M7, M33, M55, and M85.
- **C++ module support** — compile `.cpp`/`.cxx` sources with `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` and link `cpp_init_fini.c` for `__init_array` global constructor support. Call `udynlink_cpp_init()` after loading.
- **Selective symbol exporting** — `mkmodule --public-symbols func1,func2` wraps and exports only the named globals, reducing binary size and attack surface.
- **`mkmodule --bin-name <path>`** for custom output binary names.
- **`udynlink_error_msg()`** to convert error enums to human-readable strings.
- **`udynlink_get_module_name_from_image()`** to read a module name directly from a base address without fully loading.
- **`udynlink_get_image_size()`** and **`udynlink_get_text_pointer()`** for runtime introspection of loaded modules.
- **Justfile** for convenient test and build command running.
- **Multi-target QEMU test host infrastructure** with 9 test platforms including MPS2-AN386 (Cortex-M4), MPS2-AN385 (M3), MPS2-AN500 (M7), MPS2-AN505 (M33), olimex-h405 (M4F hard-float), microbit (M0), and STM32F429.
- **`UDYNLINK_SYMBOL(sym)`** convenience macro for building host symbol tables.
- **`R_ARM_GOT_PREL`** relocation support.

### Changed

- **Make LOT base address configurable** via the `UDYNLINK_LOT_BASE_ADDR` compile-time macro (previously hardcoded to `0x20000000`).
- **Remove `#include <stdio.h>` dependency** from `udynlink.c` to reduce host firmware integration friction.
- **Port build toolchain from Eclipse-generated makefiles to CMake** for standalone library builds and test host firmware.
- **Port Python 2 scripts to Python 3** and add `uv` / `pyproject.toml` dependency management for `pyelftools` and `Jinja2`.
- **Update test harness for GCC 15 / latest toolchain** compatibility.
- **Allow multiple instances of the same module** — removes deduplication by name so the same module image can be loaded more than once.
- **Enable `--gc-sections` dead code elimination** during module linking, with `KEEP` directives preserving prologues and `.init_array`.
- **Support `-O <level>` build option** in `mkmodule` (replaces `--no-opt`; accepts `0`, `s` (default), `2`, `3`, `z` like GCC).
- **Make QEMU binary and target configurable** via environment variables (`UDYNLINK_QEMU_BIN`, `UDYNLINK_QEMU_MACHINE`, `UDYNLINK_QEMU_CPU`, `UDYNLINK_QEMU_EXTRA_FLAGS`).
- **Move `.rodata` into `.data`** section to support `R_ARM_ABS32` and `R_ARM_TARGET1` data relocations.
- **Module binary layout expansion** — ABI v2.0+ header grows from 32 to 36 bytes to include `num_deps` and `deps_strtab_size` fields.

### Fixed

- **Fix `UDYNLINK_MAKE_VERSION` macro** — both components now shift by 8 instead of 16.
- **Fix multiple relocations to the same symbol in an array** — previously only the first relocation in an array was processed correctly.
- **Fix alignment issues** in module image generation.
- **Guard module unload against active dependents** via `dep_refcount` tracking.
- **Fix trivial typos** in `udynlink.h`.
- **Remove hardcoded `-fno-inline` compile flag** from module builds.
- **Isolate test builds per platform/test/optimization** for parallel execution and fix stdout blocking issues.
- **Fix MPS2-AN500 and MPS2-AN505** hosts by adding `UDYNLINK_HOST_ARCH_TAG` and switching to semihosting output.

### Infrastructure

- Migrate CI from CircleCI to GitHub Actions.
- Adopt dual-QEMU strategy: legacy `qemu-system-gnuarmeclipse` for fast STM32F429 regression testing, and mainline `qemu-system-arm` (9.2.4+) for all other platforms to ensure future compatibility.
- Add buffered SYS_WRITE0 semihosting output for MPS2-AN386 to speed up test execution.
- Build core library as `libudynlink.a` with CMake `install` targets for headers.

## Original Project (pre-0.1.0)

The original `udynlink` project was created by Bogdan Marinescu in August 2017. The [eh2k fork](https://github.com/eh2k/udynlink) (2023–2024) added C++ support, `--gc-sections`, `--public-symbols`, `R_ARM_ABS32`/`R_ARM_TARGET1` relocations, fixed LOT base, multiple module instances, and GitHub Actions CI. The current repository is an expanded continuation of the eh2k fork.
