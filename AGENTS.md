## Repository Map

A full codemap is available at `codemap.md` in the project root.

Before working on any task, read `codemap.md` to understand:
- Project architecture and entry points
- Directory responsibilities and design patterns
- Data flow and integration points between modules

For deep work on a specific folder, also read that folder's `codemap.md`.

## What This Repo Is

`udynlink` is a **micro dynamic linker for ARM Cortex-M MCUs**. It loads position-independent C/C++ binary modules at runtime on embedded targets. The core runtime is a small C library; the build toolchain is a set of Python 3 scripts that compile C/C++ to a custom loadable module format.

This repo is based on the **eh2k fork** which adds: C++ support (`__init_array`), `--gc-sections` dead code elimination, `--public-symbols` selective exporting, `R_ARM_ABS32`/`R_ARM_TARGET1` data relocations, fixed LOT base at `0x20000000`, multiple module instances, and GitHub Actions CI.

## Toolchain Requirements

- **`arm-none-eabi-gcc`** / **`arm-none-eabi-g++`** / **`arm-none-eabi-objcopy`** (GCC ARM Embedded)
- **Python 3** with `pyelftools`, `Jinja2` (managed via `uv` / `pyproject.toml`)
- **QEMU** (`qemu-system-gnuarmeclipse`, GNU MCU Eclipse flavor) for tests

All Python scripts are **Python 3** (migrated in eh2k's [1]).

## Build & Test Commands

### Build a loadable module
```bash
cd scripts
python3 mkmodule --gen-c-header --header-path /some/path source1.c [source2.c ...]
```

Additional flags:
- `--public-symbols func1,func2` — only export named symbols (reduces image size)
- `--no-opt` — compile with `-O3` instead of default `-Os`
- `--bin-name <path>` — custom output binary name
- `--build_flags=<flags>` — prepend extra compiler flags

For C++ sources (`.cpp`/`.cxx`), the toolchain automatically adds `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` and compiles `cpp_init_fini.c` for `__init_array` support.

### Run all tests
```bash
cd tests
python3 test_driver.py [test-name-prefix]
```

Without arguments, it runs every `test-*/` directory. With an argument, it runs only matching directories.

Each test is executed **twice**: once with `-O0` and once with `-Os`.

### Run a single test manually (advanced)
The test driver orchestrates several steps:
1. Compiles module C files via `../../scripts/mkmodule`
2. Copies `test_qemu.c` into `tests/qemu_host/src/`
3. Builds `test1.elf` in `tests/qemu_host/Debug/` via Eclipse-generated `makefile`
4. Runs `qemu-system-gnuarmeclipse -board STM32F429I-Discovery -image test1.elf -nographic`
5. Checks output for `*** TEST OK ***` and regex matches from `test_data.py`

### Build the test host firmware
```bash
cd tests/qemu_host/Debug
make test1.elf
```

The `Debug/` directory contains **Eclipse-generated makefiles**. Do not hand-edit `subdir.mk` or `sources.mk`; they are stamped by the IDE. The build pulls `udynlink.c` from `../../../udynlink/` via relative path.

## Architecture & Key Constraints

### Position-Independent Code Model
- Modules are compiled with `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections`
- Data access uses `r9` as a base register pointing to the **LOT** (Linker Offset Table)
- Exported functions get an assembly prologue (generated from `scripts/asm_template.tmpl`) that loads `r9` from a **fixed memory address at `0x20000000`** (RAM base on STM32F429)
- **Host must write `p_mod->ram_base` to `*(uint32_t*)0x20000000` before calling any module function** (this is the LOT base)

### Host Firmware Integration
The host MCU firmware must implement the functions in `udynlink/udynlink_externals.h`:
- `udynlink_external_malloc` / `udynlink_external_free`
- `udynlink_external_vprintf` (debug logging)
- `udynlink_external_resolve_symbol` (bind foreign symbols at load time)
- `udynlink_external_is_pointer_in_ram`

Without these, the linker will not link. `udynlink_external_resolve_symbol` is the hook that lets modules call into the host firmware or into other loaded modules.

### C++ Module Support
- Call `udynlink_cpp_init(p_mod)` after loading a C++ module to run global constructors via `__init_array`
- The host must set `*(uint32_t*)0x20000000 = p_mod->ram_base` before calling `udynlink_cpp_init`

### Module Image Format
Binary modules start with the signature `UDLM`, followed by a header, relocation table, symbol table, `.text`, and `.data`. The loader (`udynlink_load_module`) validates the signature, checks for duplicate module names, applies relocations, and resolves extern symbols.

Relocation types handled: `R_ARM_GOT_BREL` (LOT), `R_ARM_ABS32` and `R_ARM_TARGET1` (data), `R_ARM_THM_CALL`/`R_ARM_THM_JUMP24` (ignored, PC-relative).

### Three Load Modes
All tests validate all three modes by default:
- `UDYNLINK_LOAD_MODE_COPY_ALL`: copy header + text + data to RAM
- `UDYNLINK_LOAD_MODE_COPY_CODE`: copy text + data to RAM (header stays at base_addr)
- `UDYNLINK_LOAD_MODE_XIP`: copy only data to RAM; execute code in place from flash

### Module Uniqueness
Unlike the original, the eh2k fork allows **multiple instances of the same module** (no deduplication by name).

### Code Quality Note
Per the README, this code is **pre-alpha / work in progress** and "likely quite buggy."

## Known Issues (Carried Forward)

- **`#include <stdio.h>` still in `udynlink.c`** — Line 7 has `// TODO: remove next` / `#include <stdio.h>`. Inappropriate for an embedded library that should not depend on stdio.
- **`UDYNLINK_MAKE_VERSION` macro is broken** — `udynlink.h:114` shifts `major` by 8, but `UDYNLINK_GET_MAJOR_VERSION` shifts by 16. They don't round-trip.
- **Version fields still commented out** in `udynlink_module_header_t` — no ABI versioning means no way to detect module/loader incompatibility.
- **Typo** — "ownershsip" in `udynlink.h:64`, "correponding" in `udynlink.h:105`
- **No thread safety** — `module_table` is a bare static array with no locking. Cortex-M targets often use interrupts; concurrent load/unload from different interrupt levels will corrupt state.
- **Module unload doesn't verify dependents** — Unloading a module that other modules depend on via `udynlink_external_resolve_symbol` leaves dangling references.
- **`0x20000000` is hardcoded** — The LOT base address is STM32-specific. No abstraction for other MCU families with different RAM bases.
- **`UDYNLINK_MAX_HANDLES` defaults to 1** with only a `#warning` — silent default is easy to miss.
- **Test harness uses niche QEMU** — `qemu-system-gnuarmeclipse` is a specialized variant; mainstream QEMU has gained STM32 support that could replace it.

## Generated / Ignored Files

The `.gitignore` and test harness generate these artifacts; do not commit them:
- `*.o`, `*.elf`, `*.bin`, `*.hex`, `*.map`, `*.d`, `*.pyc`
- `*_module_data.h` (generated by `mkmodule --gen-c-header`)
- `tests/qemu_host/src/test_qemu.c` (copied by test driver from the active test case)
- `temp/*`

## CI

`.github/workflows/ci.yml` runs the test suite via GitHub Actions. It installs `gcc-arm-embedded`, Python 3 deps, and downloads the GNU MCU Eclipse QEMU.

## Next Steps Roadmap

| # | Task | Priority | Notes |
|---|------|----------|-------|
| 1 | Remove `#include <stdio.h>` from `udynlink.c` | High | Replace `snprintf` usage with `udynlink_external_vprintf` or manual formatting |
| 2 | Fix `UDYNLINK_MAKE_VERSION` / `UDYNLINK_GET_MAJOR_VERSION` macros | High | Shift amounts don't round-trip; decide on 8-bit or 16-bit fields |
| 3 | Uncomment and implement version fields in module header | Medium | ABI versioning prevents loading incompatible modules |
| 4 | Make LOT base address configurable (not hardcoded `0x20000000`) | Medium | Add a `udynlink_set_lot_base_addr()` API or config macro |
| 5 | Add thread safety for module table | Medium | At minimum, disable interrupts around load/unload on Cortex-M |
| 6 | Fix typos in `udynlink.h` | Low | "ownershsip" → "ownership", "correponding" → "corresponding" |
| 7 | Guard module unload against dependents | Medium | Track which modules resolve symbols from which others |
| 8 | Migrate from `qemu-system-gnuarmeclipse` to mainstream QEMU | Medium | Mainline QEMU now has STM32 support; reduces external dependency |
| 9 | Replace Eclipse-generated makefiles with CMake or Makefile | Low | Current build system is IDE-specific and not easily CI-friendly |
| 10 | Add Cortex-M0+/M3/M7 support | Low | Compilation flags hardcode `-mcpu=cortex-m4` |
| 11 | Add unit tests for Python toolchain | Low | Only integration tests via QEMU currently exist |
| 12 | Add `UDYNLINK_MAX_HANDLES` as a required compile-time constant | Low | Fail compilation if not explicitly set, instead of defaulting to 1 |
