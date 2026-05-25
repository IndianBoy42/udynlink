# Repository Atlas: udynlink

## Project Responsibility
A micro dynamic linker for ARM Cortex-M MCUs that compiles C/C++ code into position-independent binary modules and loads them at runtime from RAM or flash (XIP). Supports partial firmware updates, RAM-resident code execution, modular C/C++ plugin loading, and inter-module symbol dependencies.

This repository is the **eh2k fork** of the original udynlink project. It adds C++ support, `--gc-sections` dead code elimination, `--public-symbols` selective exporting, `R_ARM_TARGET1` relocation support, and GitHub Actions CI.

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
