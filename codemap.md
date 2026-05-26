# Repository Atlas: udynlink

## Project Responsibility
A micro dynamic linker for ARM Cortex-M MCUs that compiles C/C++ code into position-independent binary modules and loads them at runtime from RAM or flash (XIP). Supports partial firmware updates, RAM-resident code execution, modular C/C++ plugin loading, inter-module symbol dependencies, hash-based O(1) symbol resolution, and dependency tracking with safe unload.

This repository is the **eh2k fork** of the original udynlink project. It adds C++ support, `--gc-sections` dead code elimination, `--public-symbols` selective exporting, `R_ARM_TARGET1` relocation support, hash-based symbol resolution, module dependency tracking, and GitHub Actions CI.

## System Entry Points
- `udynlink/udynlink.h`: Public C API for host firmware integration.
- `udynlink/udynlink.c`: Core runtime loader implementation.
- `scripts/mkmodule`: CLI toolchain entry point for building loadable modules.
- `README.md`: Architecture overview, usage guide, and changelog.
- `.github/workflows/ci.yml`: GitHub Actions CI (GCC ARM Embedded + QEMU test runner).

## Directory Map (Aggregated)
| Directory | Responsibility Summary | Detailed Map |
|-----------|------------------------|--------------|
| `udynlink/` | Core dynamic linker runtime: module loading, relocation, symbol resolution, unloading, C++ constructor init. | [View Map](udynlink/codemap.md) |
| `scripts/` | Build toolchain: compile C/C++ to PIC, wrap exports, link with `--gc-sections`, parse ELF, emit loadable binary images. | [View Map](scripts/codemap.md) |
| `tests/` | QEMU-based integration test harness: test driver, per-test modules, host firmware, and validation utils. | [View Map](tests/codemap.md) |

## Key Design Notes
- **Position Independence**: Relies on GCC ARM Embedded flags (`-fPIE`, `-msingle-pic-base`) and an `r9`-relative LOT (Linker Offset Table) instead of a traditional GOT.
- **External Symbol Resolution**: Host must provide `udynlink_external_resolve_symbol` to bind foreign symbols at load time, enabling inter-module dependencies.
- **Load Modes**: Three modes supported (`COPY_ALL`, `COPY_CODE`, `XIP`) trade RAM usage vs. execution flexibility.
- **Module Identity**: Signature `UDLM` + module name symbol enforce uniqueness at load time.
- **C++ Support**: The eh2k fork adds `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` compilation, `__init_array` constructor invocation via `udynlink_cpp_init()`, and a `cpp_init_fini.c` runtime helper.
- **Fixed LOT Base**: The original `udynlink_get_lot_base(pc)` function pointer at address `0x1c` was replaced by a fixed memory location at `0x20000000` (RAM base). The host must write `p_mod->ram_base` to `*(uint32_t*)0x20000000` before calling any module function.
- **Dead Code Elimination**: `--gc-sections` is used during linking, with `KEEP(*(.text_nogc))` and `KEEP(*(.init_array))` preserving prologues and constructors.
- **Selective Exporting**: `--public-symbols` allows restricting which global functions are wrapped/exported, reducing binary size and attack surface.

### Hash-Based Symbol Resolution (O(1))
- New files: `udynlink/udynlink_hash.h` (hash table struct + lookup declaration) and `udynlink/udynlink_hash.c` (~60-line GNU hash + bloom filter lookup implementation)
- New tool: `scripts/mkhostsyms` — Python tool that reads a host firmware ELF, generates a C header with const hash table data for O(1) symbol resolution
- The hash table struct `udynlink_hash_table_t` contains: bloom filter, buckets, hash values, symbol addresses, and string table
- Lookup function: `udynlink_resolve_hashed_symbol()` — O(1) amortized, replaces the O(N) strcmp resolution chain
- No changes to existing `udynlink.h` / `udynlink.c` / `udynlink_externals.h` for this feature; it is an optional additive capability

### Module Dependency Tracking
- Modified files: `udynlink/udynlink.h` (header struct grew from 32 to 36 bytes, module struct extended, new error codes, ABI version bump to 2.0), `udynlink/udynlink.c` (dependency validation, 3-tier extern symbol resolution, safe unload), `udynlink/udynlink_externals.h` (2 new callbacks)
- New test: `tests/test-deps/` with provider + consumer modules
- UDLM header now 36 bytes with `num_deps` and `deps_strtab_size` fields; binary layout: [Header 36B] [Relocs] [Symtab] [Deps strtab] [Code] [Data]
- `mkmodule --depends mod_a,mod_b` declares module dependencies at build time
- Three-tier symbol resolution: critical host symbols → loaded dependency modules → fallback host symbols
- Safe unload: modules with active dependents (tracked via `dep_refcount`) cannot be unloaded until all dependents are removed
- v1.0 backward compatibility: `get_header_size()` returns 32 for v1.0, 36 for v2.0+

### Streaming I/O Module Loading
- New API: `udynlink_load_module_stream()` loads modules from a `udynlink_io_t` callback interface (random-access `read` + `get_size`), enabling loading from SD card, SPI flash, network streams, or any non-memory-mapped source without pre-buffering the entire image
- Caller provides a work buffer (minimum 64 bytes, optimal size from `udynlink_get_stream_metadata_size()`); the loader reads metadata and copies sections through this buffer in chunks
- On-demand symbol resolution: symbol entries and names are read from the stream during relocation processing, avoiding pre-loading the entire symbol table into RAM
- Supports COPY_ALL and COPY_CODE modes; XIP returns `UDYNLINK_ERR_LOAD_UNABLE_TO_XIP`
- For COPY_CODE streaming, the loader uses a COPY_ALL-style RAM layout so that `udynlink_lookup_symbol` works after loading
- Query functions: `udynlink_get_ram_requirements()`, `udynlink_get_ram_requirements_stream()`, `udynlink_get_stream_metadata_size()`
- New error code: `UDYNLINK_ERR_LOAD_IO_ERROR` for stream read failures
- New helper macro: `UDYNLINK_SYMBOL(sym)` for building host symbol tables
- New test: `tests/test-streaming-load/` with mock `udynlink_io_t` wrapping memory-mapped data
- `uintptr_t` cleanup: all raw `(uint32_t)ptr` casts replaced with `(uint32_t)(uintptr_t)ptr` to suppress 64-bit host warnings

## Changelog Summary (eh2k fork)
- `[12]` 2024-12-01: `--gc-sections` + readonly data & reloc optimizations
- `[11]` 2024-09-25: Added `udynlink_get_module_size`, `udynlink_get_code_pointer`
- `[10]` 2024-09-25: Fixed multiple relocations to same symbol in arrays
- `[9]` 2023-11-12: Removed hardcoded `-fno-inline`, added `-fno-rtti` for C++
- `[8]` 2023-11-10: `udynlink_cpp_init` for C++ global constructors
- `[7]` 2023-11-08: `--public-symbols` flag to write only public symbols
- `[5]` 2023-11-06: `--bin-name` arg, `udynlink_error_msg`, `udynlink_get_module_name2`
- `[4]` 2023-11-02: `R_ARM_ABS32` data relocation support, `-O3` option
- `[3]` 2023-11-01: Fixed LOT base at `0x20000000`, no module reuse
- `[2]` 2023-11-01: C++ compilation support (`-fno-exceptions`), `--build_flags`
- `[1]` 2023-11-01: Python 3 migration, CircleCI -> GitHub Actions, latest QEMU
