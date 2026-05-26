# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-05-26

### Added

- **Streaming I/O module loading** — load modules from SD card, SPI flash, or any non-memory-mapped source via the `udynlink_io_t` callback interface without pre-buffering the entire image. Introduces `udynlink_load_module_stream()`, `udynlink_get_ram_requirements_stream()`, and `udynlink_get_stream_metadata_size()`.
- **Hash-based O(1) symbol resolution** — optional GNU hash table for host firmware symbol lookup. Adds `udynlink_hash.h`/`udynlink_hash.c` and the `scripts/mkhostsyms` tool that generates a const hash table from a host ELF.
- **Module dependency tracking** — declare dependencies at build time with `mkmodule --depends mod_a,mod_b`. The loader enforces that all declared dependencies are already loaded. Three-tier symbol resolution: critical host symbols → dependency modules → fallback host symbols. Safe unload via `dep_refcount` prevents unloading a module that has active dependents.
- **ABI versioning and architecture tag validation** — module headers include `mod_version`, `udynlink_version`, and `arch_tag`. The loader validates compatibility at load time and rejects modules compiled for a mismatched core family, FPU, or float ABI.
- **Compile-time target database** (`scripts/targets.py`) with per-target assembly prologue templates supporting Cortex-M0, M0+, M3, M4, M4F, M7, M33, M55, and M85.
- **C++ module support** — compile `.cpp`/`.cxx` sources with `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` and link `cpp_init_fini.c` for `__init_array` global constructor support. Call `udynlink_cpp_init()` after loading.
- **Selective symbol exporting** — `mkmodule --public-symbols func1,func2` wraps and exports only the named globals, reducing binary size and attack surface.
- **`mkmodule --bin-name <path>`** for custom output binary names.
- **`udynlink_error_msg()`** to convert error enums to human-readable strings.
- **`udynlink_get_module_name2()`** to read a module name directly from a base address without fully loading.
- **`udynlink_get_module_size()`** and **`udynlink_get_code_pointer()`** for runtime introspection of loaded modules.
- **Justfile** for convenient test and build command running.
- **Multi-target QEMU test host infrastructure** with 9 test platforms including MPS2-AN386 (Cortex-M4), MPS2-AN385 (M3), MPS2-AN500 (M7), MPS2-AN505 (M33), olimex-h405 (M4F hard-float), microbit (M0), and STM32F429.
- **`UDYNLINK_SYMBOL(sym)`** convenience macro for building host symbol tables.
- **`R_ARM_GOT_PREL`** relocation support.

### Changed

- **Make LOT base address configurable** via the `UDYNLINK_LOT_BASE_ADDR` compile-time macro (previously hardcoded to `0x20000000`).
- **Make `UDYNLINK_MAX_HANDLES` a required compile-time constant** — emits `#error` if unset instead of silently defaulting to 1.
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
