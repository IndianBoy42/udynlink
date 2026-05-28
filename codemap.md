# Repository Atlas: udynlink

## Project Responsibility
A micro dynamic linker for ARM Cortex-M MCUs that compiles C/C++ code into position-independent binary modules and loads them at runtime from RAM or flash (XIP). Supports partial firmware updates, RAM-resident code execution, modular C/C++ plugin loading, hash-based O(1) symbol resolution, and low-level linking primitives.

This repository is the **eh2k fork** of the original udynlink project. It adds C++ support, `--gc-sections` dead code elimination, `--public-symbols` selective exporting, `R_ARM_TARGET1` relocation support, hash-based symbol resolution, and GitHub Actions CI.

## System Entry Points
- `udynlink/udynlink.h`: Public C API for host firmware integration.
- `udynlink/udynlink.c`: Core runtime loader implementation.
- `scripts/mkmodule`: CLI toolchain entry point for building loadable modules.
- `README.md`: Architecture overview, usage guide, and changelog.
- `.github/workflows/ci.yml`: GitHub Actions CI (GCC ARM Embedded + QEMU test runner).

## Directory Map (Aggregated)
| Directory | Responsibility Summary | Detailed Map |
|-----------|------------------------|--------------|
| `udynlink/` | Core dynamic linker runtime: module loading, relocation, symbol resolution, unloading, C++ constructor init, cross-module thunk pool. | [View Map](udynlink/codemap.md) |
| `scripts/` | Build toolchain: compile C/C++ to PIC, wrap exports, link with `--gc-sections`, parse ELF, emit loadable binary images. | [View Map](scripts/codemap.md) |
| `tests/` | QEMU-based integration test harness: test driver, per-test modules (including cross-module deps), host firmware, and validation utils. | [View Map](tests/codemap.md) |

## Key Design Notes
- **Position Independence**: Relies on GCC ARM Embedded flags (`-fPIE`, `-msingle-pic-base`) and an `r9`-relative LOT (Linker Offset Table) instead of a traditional GOT.
- **External Symbol Resolution**: Host must provide `udynlink_external_resolve_symbol` to bind foreign symbols at load time.
- **Load Modes**: Three modes supported (`COPY_ALL`, `COPY_TEXT_DATA`, `XIP`) trade RAM usage vs. execution flexibility.
- **Module Identity**: Signature `UDLM` + module name symbol enforce uniqueness at load time.
- **C++ Support**: The eh2k fork adds `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` compilation, `__init_array` constructor invocation via `udynlink_cpp_init()`, and a `cpp_init_fini.c` runtime helper. The host must prepare `r9` via `UDYNLINK_PREPARE_CALL()` before calling `udynlink_cpp_init()`.
- **Dead Code Elimination**: `--gc-sections` is used during linking, with `KEEP(*(.text_nogc))` and `KEEP(*(.init_array))` preserving prologues and constructors.
- **Selective Exporting**: `--public-symbols` allows restricting which global functions are wrapped/exported, reducing binary size and attack surface.
- **Host-Managed r9**: ABI v3.0 removed `UDYNLINK_LOT_BASE_ADDR`. The host sets `r9` directly via `UDYNLINK_PREPARE_CALL(p_mod)` before every module call. The assembly prologue only saves/restores the caller's `r9`.
- **Cross-Module Calls**: The `udynlink_deps` layer adds optional cross-module linking via runtime-generated thunks in executable RAM. `udynlink/udynlink_deps.h` and `udynlink/udynlink_deps.c` implement the thunk pool, dependency manager, and resolution helpers. The `UDYNLINK_REQUIRES` macro lets modules declare explicit dependencies.

### Hash-Based Symbol Resolution (O(1))
- New files: `udynlink/udynlink_hash.h` (hash table struct + lookup declaration + ~60-line GNU hash + bloom filter lookup implementation)
- New tool: `scripts/mkhostsyms` — Python tool that reads a host firmware ELF, generates a C header with const hash table data for O(1) symbol resolution
- The hash table struct `udynlink_hash_table_t` contains: bloom filter, buckets, hash values, symbol addresses, and string table
- Lookup function: `udynlink_resolve_hashed_symbol()` — O(1) amortized, replaces the O(N) strcmp resolution chain
- No changes to existing `udynlink.h` / `udynlink.c` / `udynlink_externals.h` for this feature; it is an optional additive capability

### Cross-Module Dependency System (`udynlink_deps`)
- New files: `udynlink/udynlink_deps.h` (public API) and `udynlink/udynlink_deps.c` (implementation)
- Thunk pool: `udynlink_thunk_pool_t` / `udynlink_thunk_pool_init()` / `udynlink_thunk_alloc()` — bump allocator in executable RAM
- Dependency manager: `udynlink_dep_mgr_t` / `udynlink_dep_mgr_init()` — tracks loaded modules, detects circular dependencies (max depth 8)
- Resolution helpers: `udynlink_dep_resolve_dependency()`, `udynlink_dep_resolve_func()`, `udynlink_dep_resolve_data()` — called from host's `udynlink_external_resolve_symbol()`
- Load/unload wrappers: `udynlink_dep_load()` / `udynlink_dep_unload()` — auto-register modules, push/pop loading stack
- Thunk template: 28-byte ARM Thumb-2 inline function that saves caller's `r9`, sets callee's `r9` via `ram_base`, calls target via `blx ip`, then restores caller's `r9`
- Uses `r12` (IP) for target function address to avoid clobbering `r0-r3` argument registers
- Test: `tests/test-cross-module/` — validates cross-module calls between `mod_math` and `mod_app` across all load modes and optimization levels

### Non-Contiguous Image Loading
- New API: `udynlink_load_module_image()` loads modules from a `udynlink_module_image_t` descriptor with per-section pointers, enabling loading from SD card, SPI flash, decompressed buffers, or any non-contiguous source
- `udynlink_image_from_memory()` and `udynlink_image_from_module()` build the descriptor from a contiguous UDLM buffer or an already-loaded module handle
- Low-level primitives for custom pipelines: `udynlink_validate_header()`, `udynlink_compute_ram_size()`, `udynlink_get_image_metadata_size()`, `udynlink_image_get_module_name()`, and `udynlink_load_apply_relocations()`
- The relocation engine is fully decoupled from source layout: `udynlink_load_apply_relocations()` takes raw pointers to the header, relocation table, and symbol table, then patches the module's RAM
- Both `udynlink_load_module()` (contiguous memory) and `udynlink_load_module_image()` (non-contiguous descriptor) share a single canonical relocation path via the same internal helpers
- Query functions: `udynlink_get_ram_requirements()` (wrapper around `udynlink_compute_ram_size()`)
- New test: `tests/test-streaming-load/` renamed to exercise `udynlink_load_module_image()` for all load modes plus planning/validation APIs
- `uintptr_t` cleanup: all raw `(uint32_t)ptr` casts replaced with `(uint32_t)(uintptr_t)ptr` to suppress 64-bit host warnings

## Changelog Summary (eh2k fork)
- `[12]` 2024-12-01: `--gc-sections` + readonly data & reloc optimizations
- `[11]` 2024-09-25: Added `udynlink_get_image_size`, `udynlink_get_text_pointer`
- `[10]` 2024-09-25: Fixed multiple relocations to same symbol in arrays
- `[9]` 2023-11-12: Removed hardcoded `-fno-inline`, added `-fno-rtti` for C++
- `[8]` 2023-11-10: `udynlink_cpp_init` for C++ global constructors
- `[7]` 2023-11-08: `--public-symbols` flag to write only public symbols
- `[5]` 2023-11-06: `--bin-name` arg, `udynlink_error_msg`, `udynlink_get_module_name_from_image`
- `[4]` 2023-11-02: `R_ARM_ABS32` data relocation support, `-O3` option
- `[3]` 2023-11-01: Fixed LOT base at `0x20000000`, no module reuse
- `[2]` 2023-11-01: C++ compilation support (`-fno-exceptions`), `--build_flags`
- `[1]` 2023-11-01: Python 3 migration, CircleCI -> GitHub Actions, latest QEMU
