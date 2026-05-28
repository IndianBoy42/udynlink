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

Recent expansion adds: ABI versioning with `mod_version`/`udynlink_version`/`arch_tag`, architecture tag validation at load time, a Python target database (`scripts/targets.py`) supporting Cortex-M0/M0+/M3/M4/M4F/M7/M33/M55/M85, and per-target assembly prologue templates.

## Documentation Structure

All user-facing documentation lives under `docs/` and is summarized in `docs/README.md`:

| Guide | Description |
|-------|-------------|
| `docs/how-it-works.md` | Technical deep-dive: PIC model, LOT/r9 mechanism, relocations, binary format, ABI versioning |
| `docs/integrating-as-host.md` | Adding udynlink to your firmware, implementing callbacks, symbol tables, lifecycle, thread safety |
| `docs/writing-modules.md` | Creating loadable C/C++ modules, consuming symbols, mkmodule reference |
| `docs/api-reference.md` | Complete reference for all public functions, structs, macros, and callbacks |
| `docs/examples.md` | Working code examples for every major feature |
| `docs/testing.md` | Running tests, adding test cases and platforms, debugging |

> **Always keep documentation in sync.** If you change code, public APIs, test behavior, build commands, or toolchain requirements, update the corresponding `docs/*.md` file(s) before finishing the task. `AGENTS.md` itself must also be updated if build/test commands, architecture constraints, or the platform matrix change.

## Toolchain Requirements

- **`arm-none-eabi-gcc`** / **`arm-none-eabi-g++`** / **`arm-none-eabi-objcopy`** (GCC ARM Embedded)
- **CMake** ≥ 3.16
- **Python 3** with `pyelftools`, `Jinja2` (managed via `uv` / `pyproject.toml`)
- **QEMU** for tests. We test against two QEMU variants:
  - **Mainline QEMU** (`qemu-system-arm` 9.2.4+): Used for MPS2-AN3xx/AN5xx and Olimex platforms. Usually available via your distro package manager (`apt install qemu-system-arm`, `brew install qemu`, etc.).
  - **Legacy xPack QEMU** (`qemu-system-gnuarmeclipse`): Used for STM32F4xx discovery boards (fastest). This is the legacy GNU MCU Eclipse fork.

  **Quick setup:**
  - Run `just setup-qemu` to download the latest xPack release (mainline `qemu-system-arm`) into `tests/`.
  - Run `just setup-qemu-legacy` to download the last xPack release that includes `qemu-system-gnuarmeclipse` (7.2.5-1) into `tests/`.
  - The Justfile will automatically prefer these local copies over system-wide installations.

  **Manual setup:** Download the appropriate xPack tarball from https://github.com/xpack-dev-tools/qemu-arm-xpack/releases and extract it to `tests/`. The latest recommended version is 9.2.4-1 for mainline QEMU, and 7.2.5-1 for the legacy fork.

  The test harness supports any QEMU binary via `UDYNLINK_QEMU_BIN` and `UDYNLINK_QEMU_LEGACY_BIN` environment variables.

**Optional tools:**
- **`scripts/mkhostsyms`** — reads a host firmware ELF and generates a C header with a const GNU hash table for O(1) symbol resolution (see Hash-Based Symbol Resolution below)

All Python scripts are **Python 3** (migrated in eh2k's [1]).

## Build & Test Commands

### Build the core library with CMake
The core `udynlink` library can be built standalone for use in downstream projects:
```bash
cmake -B build -S .
cmake --build build
```

This produces `build/libudynlink.a` and install targets for headers (`udynlink.h`, `udynlink_externals.h`). Downstream projects can consume it via `add_subdirectory()` or `find_package(udynlink)` after install.

### Build a loadable module
```bash
cd scripts
python3 mkmodule --gen-c-header --header-path /some/path source1.c [source2.c ...]
```

Additional flags:
- `--public-symbols func1,func2` — only export named symbols (reduces image size)
- `-O <level>` — optimization level (`0`, `s`, `2`, `3`, `z`; default: `s`)
- `--bin-name <path>` — custom output binary name
- `--build-flags=<flags>` — prepend extra compiler flags
- `--mcpu <cpu>` — target CPU (default: `cortex-m4`)
- `--target <name>` — target from the target database (default: `cortex-m4`). Supported: `cortex-m0`, `cortex-m0plus`, `cortex-m3`, `cortex-m4`, `cortex-m4f`, `cortex-m7`, `cortex-m33`, `cortex-m55`, `cortex-m85`
- `--mod-version <ver>` — module ABI version (default: `1.0`)
- `--udynlink-version <ver>` — loader ABI version (default: `3.0`)

For C++ sources (`.cpp`/`.cxx`), the toolchain automatically adds `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` and compiles `cpp_init_fini.c` for `__init_array` support.

The compiler prefix can be overridden via the `UDYNLINK_CC_PREFIX` environment variable (default: `arm-none-eabi-`).

### Run all tests (via `just` — recommended)

**Always use `just` for running tests.** The `Justfile` encodes the correct QEMU flags,
module targets, and timeouts for each platform. Running `test_driver.py` manually with
ad-hoc environment variables is not supported and will likely fail.

```bash
just test-mps2          # MPS2-AN386 (Cortex-M4) — mainline QEMU
just test-an385         # MPS2-AN385 (Cortex-M3)
just test-an500         # MPS2-AN500 (Cortex-M7)
just test-an505         # MPS2-AN505 (Cortex-M33)
just test-h405          # Olimex STM32-H405 (Cortex-M4F hard-float)
just test-f429          # STM32F429 (legacy xPack QEMU — fast)
just test-f429-single test-globals1   # Single test on STM32F429
```

Each test is executed **twice**: once with `-O0` and once with `-Os`.
The full suite of 24 tests per platform completes in ~30 seconds (mainline QEMU).

### Run a single test manually (advanced)
The test driver orchestrates several steps:
1. Compiles module C files via `../../scripts/mkmodule`
2. Copies `test_qemu.c` into `tests/qemu_host/src/`
3. Builds `test1.elf` in the CMake build directory:
   ```bash
   cmake -B tests/build -S tests/qemu_host -DUDYNLINK_BUILD_TESTS=ON
   cmake --build tests/build --target test1.elf
   ```
4. Runs QEMU with the freshly compiled `test1.elf`
    - Default: `qemu-system-gnuarmeclipse -board STM32F429I-Discovery -image test1.elf -nographic`
    - Override via env vars: `UDYNLINK_QEMU_BIN`, `UDYNLINK_QEMU_MACHINE`, `UDYNLINK_QEMU_CPU`, `UDYNLINK_QEMU_EXTRA_FLAGS`
5. Checks output for `*** TEST OK ***` and regex matches from `test_data.py`

### Build a loadable module
```bash
cd scripts
python3 mkmodule --gen-c-header --header-path /some/path source1.c [source2.c ...]
```

Additional flags:
- `--public-symbols func1,func2` — only export named symbols (reduces image size)
- `-O <level>` — optimization level (`0`, `s`, `2`, `3`, `z`; default: `s`)
- `--bin-name <path>` — custom output binary name
- `--build-flags=<flags>` — prepend extra compiler flags
- `--mcpu <cpu>` — target CPU (default: `cortex-m4`)
- `--target <name>` — target from the target database (default: `cortex-m4`). Supported: `cortex-m0`, `cortex-m0plus`, `cortex-m3`, `cortex-m4`, `cortex-m4f`, `cortex-m7`, `cortex-m33`, `cortex-m55`, `cortex-m85`
- `--mod-version <ver>` — module ABI version (default: `1.0`)
- `--udynlink-version <ver>` — loader ABI version (default: `3.0`)

For C++ sources (`.cpp`/`.cxx`), the toolchain automatically adds `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` and compiles `cpp_init_fini.c` for `__init_array` support.

The compiler prefix can be overridden via the `UDYNLINK_CC_PREFIX` environment variable (default: `arm-none-eabi-`).

### Other `just` commands

```bash
just --list                    # Show all available commands
just help                      # Show detailed help with examples

# Build
just build-lib                 # Build core library
just build-tests               # Build tests (default: stm32f429_discovery)
just build-tests mps2_an386    # Build for MPS2-AN386 platform

# Module compilation
just module source.c            # Compile module for default target
just module-for cortex-m7 source.c   # Compile for specific target
just targets                   # List all supported targets
just target-info cortex-m4f    # Show target details

# Validation
just validate-all-targets      # Compile hello.c for all 9 targets
just ci                        # Full CI suite (MPS2 + AN385 + AN500 + AN505 + H405)
```

### Build the test host firmware

**In-tree** (from repo root, builds the core library as a dependency):
```bash
cmake -B build -S . -DUDYNLINK_BUILD_TESTS=ON
cmake --build build --target test1.elf
```

**Standalone** (from `tests/qemu_host/`, builds the core library automatically):
```bash
cmake -B tests/build -S tests/qemu_host -DUDYNLINK_BUILD_TESTS=ON
cmake --build tests/build
```

The platform is selected via `-DUDYNLINK_PLATFORM=<name>` (default: `stm32f429_discovery`), which loads the corresponding file from `tests/platforms/<name>/`.

## New Headers

- `udynlink/udynlink_call.h` — C convenience layer for host-call ergonomics. Provides `udynlink_func_t`, `udynlink_resolve_func()`, `UDYNLINK_CALL`, and `UDYNLINK_CALL_MODULE_FUNC`. These macros save and restore the host's `r9` around module calls.
- `udynlink/udynlink_deps.h` — Optional standalone dependency system for cross-module function calls and dependency tracking. Provides `UDYNLINK_REQUIRES`, `udynlink_thunk_pool_t`, `udynlink_dep_mgr_t`, `udynlink_dep_resolve_func()`, `udynlink_dep_resolve_data()`, and load/unload wrappers with circular-dependency detection. See [Integrating as a Host](docs/integrating-as-host.md) for usage.

## Architecture & Key Constraints

### Position-Independent Code Model
- Modules are compiled with `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections`
- Data access uses `r9` as a base register pointing to the **LOT** (Linker Offset Table)
- Exported functions get an assembly prologue (generated from `scripts/asm_template_*.tmpl`) that saves the caller's `r9`, loads `r9` with the module's LOT base (`p_mod->ram_base`), and restores the original `r9` on return
- **Host must use `UDYNLINK_PREPARE_CALL(p_mod)` before calling any module function** (or use the `UDYNLINK_CALL` macro which handles this automatically)
- The `--no-prologue` flag skips the assembly wrapper and sets `UDYNLINK_ARCH_FLAG_NO_PROLOGUE` in the module header. The host must use `UDYNLINK_PREPARE_CALL()` to set `r9` directly for such modules.

### Host Firmware Integration
The host MCU firmware must implement the functions in `udynlink/udynlink_externals.h`:
- `udynlink_external_malloc` / `udynlink_external_free`
- `udynlink_external_vprintf` (debug logging)
- `udynlink_external_resolve_symbol` (bind foreign symbols at load time)
- `udynlink_external_is_pointer_in_ram`

Without these, the linker will not link. `udynlink_external_resolve_symbol` is the hook that lets modules call into the host firmware.

### Weak Symbol Support
`__attribute__((weak))` symbols defined in a module are classified as `UDYNLINK_SYM_TYPE_WEAK`. At load time the loader applies the module's own address as the default, then attempts host override via `udynlink_external_resolve_symbol()`. If no override is found, the module's definition remains — the load does **not** fail.

**Important limitation:** direct internal calls (`bl`) are PC-relative and resolved at link time.  The generated prologue wrapper unconditionally branches to the module's renamed local implementation, so **internal callers always use the module's own weak definition**.  Host override is only effective for:
- **Data weak symbols** — LOT/data relocations are patched at load time
- **Function pointers / indirect calls** — address taken through the GOT
- **External callers** — `udynlink_lookup_symbol()` resolves the override at runtime

Undefined weak symbols (`STB_WEAK` + `SHN_UNDEF`) are treated as `external` and still fail module load if unresolved.  Standard ELF silently resolves them to NULL; a dedicated `weak_undef` type could be added in a follow-up.

### C++ Module Support
- Call `udynlink_cpp_init(p_mod)` after loading a C++ module to run global constructors via `__init_array`
- The host must set `r9` to the module's LOT base (`p_mod->ram_base`) before calling `udynlink_cpp_init`, using `UDYNLINK_PREPARE_CALL(p_mod)` or `UDYNLINK_CALL`

### Module Image Format
Binary modules start with the signature `UDLM`, followed by a 32-byte header, relocation table, symbol table, `.text`, and `.data`. The loader (`udynlink_load_module`) validates the signature, checks ABI version and architecture tag compatibility, applies relocations, and resolves extern symbols.

Binary layout: [Header 32B] [Relocs] [Symtab] [Code] [Data]

Relocation types handled: `R_ARM_GOT_BREL` (LOT), `R_ARM_ABS32` and `R_ARM_TARGET1` (data), `R_ARM_THM_CALL`/`R_ARM_THM_JUMP24` (ignored, PC-relative).

The header contains `mod_version`, `udynlink_version`, and `arch_tag` fields for runtime compatibility checking. `arch_tag` encodes the core family, FPU presence, and float ABI.

### Three Load Modes
All tests validate all three modes by default:
- `UDYNLINK_LOAD_MODE_COPY_ALL`: copy header + text + data to RAM
- `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA`: copy text + data to RAM (header stays at base_addr)
- `UDYNLINK_LOAD_MODE_XIP`: copy only data to RAM; execute code in place from flash

### Dependency System (`udynlink_deps`)
The optional `udynlink/udynlink_deps.h` layer provides cross-module function calls via runtime-generated RAM thunks. Each cross-module function reference gets a 28-byte inline thunk that saves/restores the caller's `r9` and calls the target with the callee module's `ram_base`. The thunk uses `r12` (IP) for the function address to avoid clobbering argument registers `r0-r3`. The thunk pool must be in executable RAM.

- Modules declare dependencies with `UDYNLINK_REQUIRES(mod_name)` which emits a `.udynlink.mod.requires.{name}` symbol.
- The host's `udynlink_external_resolve_symbol()` checks `udynlink_dep_is_dependency()` first, then `udynlink_dep_resolve_func()` for functions, then `udynlink_dep_resolve_data()` for data, and finally falls back to host-native symbols.
- `udynlink_dep_load()` and `udynlink_dep_unload()` wrap the core loader with automatic module registration and circular-dependency detection (max depth 8).

### Module Uniqueness
Unlike the original, the eh2k fork allows **multiple instances of the same module** (no deduplication by name).

## Testing Platform Matrix

| Platform | QEMU Machine | QEMU Binary | CPU | Status | Notes |
|----------|--------------|-------------|-----|--------|-------|
| `stm32f429_discovery` | STM32F429I-Discovery | `qemu-system-gnuarmeclipse` | cortex-m4 | ✅ **All 48 tests pass** | Fast, legacy xPack fork (46/48 pass; `test-strip-init-array` skipped) |
| `mps2_an386` | mps2-an386 | `qemu-system-arm` (9.2.4+) | cortex-m4 | ✅ **All 48 tests pass** | Mainline QEMU, ~0.5s/test |
| `olimex_stm32_h405` | olimex-stm32-h405 | `qemu-system-arm` | cortex-m4f | ✅ **All 48 tests pass** | Hard-float M4F on mainline QEMU |
| `mps2_an385` | mps2-an385 | `qemu-system-arm` | cortex-m3 | ✅ **All 48 tests pass** | Mainline QEMU |
| `mps2_an500` | mps2-an500 | `qemu-system-arm` | cortex-m7 | ✅ **All 48 tests pass** | Mainline QEMU |
| `mps2_an505` | mps2-an505 | `qemu-system-arm` | cortex-m33 | ✅ **All 48 tests pass** | Mainline QEMU, secure boot (see notes) |
| `microbit` | microbit | `qemu-system-arm` | cortex-m0 | ⚠️ **Builds, `-kernel` broken** | QEMU microbit machine does not support ELF `-kernel` at 0x00000000 |
| `stm32f103_bluepill` | NUCLEO-F103RB | `qemu-system-gnuarmeclipse` | cortex-m3 | ⚠️ **Boots, internal calls OK** | Flash→RAM host calls hang (QEMU quirk) |
| `stm32f051_discovery` | STM32F0-Discovery | `qemu-system-gnuarmeclipse` | cortex-m0 | ⚠️ **Boots, internal calls OK** | Same Flash→RAM quirk as M3 |

**Dual-QEMU Strategy:**
- **STM32F429** (legacy xPack `qemu-system-gnuarmeclipse`): Fast baseline/regression testing
- **MPS2-AN386** (mainline `qemu-system-arm` 9.2.4+): Future-proof, validates no xPack-specific bugs
- All other platforms target mainline QEMU for future compatibility

**MPS2-AN505 (Cortex-M33) Note:**
QEMU boots the Cortex-M33 in **Secure state** and fetches the initial vector table from the secure alias address `0x10000000`. The `tests/platforms/mps2_an505/mem.ld` linker script places the vector table at `0x10000000` so `-kernel` loading works directly.

**microbit Note:**
QEMU's `microbit` machine does not properly load ELF files via `-kernel` at `0x00000000`. It needs a raw binary loaded via `-device loader,file=...,addr=0x0`. The test harness currently does not support this.

## Known Issues (Carried Forward)

- **No thread safety** — `module_table` is a bare static array with no locking. Cortex-M targets often use interrupts; concurrent load/unload from different interrupt levels will corrupt state. (OUT OF SCOPE)
- **M3/M0 QEMU hosts have Flash→RAM call quirk** — Modules calling host functions (e.g. `printf`) hang under `qemu-system-gnuarmeclipse` for STM32F103/STM32F051 boards, but work correctly on STM32F429. This is a known `qemu-system-gnuarmeclipse` emulation bug; mainline QEMU (`qemu-system-arm`) does **not** exhibit this issue.
- **F429 semihosting heap corruption** — `qemu-system-gnuarmeclipse` 2.8.0 corrupts heap-allocated guest RAM at low addresses (0x20000400–0x20000600 range) during semihosting `SYS_WRITE0` calls. This causes `test-strip-init-array` to fail on the F429 platform. All other platforms (MPS2, H405, etc.) using mainline `qemu-system-arm` are unaffected. The test is skipped on `stm32f429_discovery` via `skip_platforms` in `test_data.py`.
- **xPack QEMU 9.2.4 discontinued `qemu-system-gnuarmeclipse`** — Latest xPack releases only include `qemu-system-arm` (mainline). STM32F429 fast testing requires an older xPack release or the `xpack-dev-tools/qemu-arm` project.


## Generated / Ignored Files

The `.gitignore` and test harness generate these artifacts; do not commit them:
- `*.o`, `*.elf`, `*.bin`, `*.hex`, `*.map`, `*.d`, `*.pyc`
- `*_module_data.h` (generated by `mkmodule --gen-c-header`)
- `tests/qemu_host/src/test_qemu.c` (copied by test driver from the active test case)
- `temp/*`

## CI

`.github/workflows/ci.yml` runs the test suite via GitHub Actions. It installs `gcc-arm-embedded`, CMake, Python 3 deps, and QEMU.

## Next Steps Roadmap

| # | Task | Priority | Notes |
|---|------|----------|-------|
| 1 | ~~Remove `#include <stdio.h>` from `udynlink.c`~~ | ~~High~~ | Done |
| 2 | ~~Fix `UDYNLINK_MAKE_VERSION` / `UDYNLINK_GET_MAJOR_VERSION` macros~~ | ~~High~~ | Done |
| 3 | ~~Uncomment and implement version fields in module header~~ | ~~Medium~~ | Done |
| 4 | ~~Make LOT base address configurable (not hardcoded `0x20000000`)~~ | ~~Medium~~ | Removed in v3.0. Host now manages `r9` directly via `UDYNLINK_PREPARE_CALL`. |
| 5 | ~~Add thread safety for module table~~ | ~~Medium~~ | Done. Full interrupt-disable around load/unload is still TODO. |
| 6 | ~~Fix typos in `udynlink.h`~~ | ~~Low~~ | Done |
| 7 | ~~Guard module unload against dependents~~ | ~~Medium~~ | Removed in v3.0 along with the built-in dependency system. |
| 8 | ~~Migrate from `qemu-system-gnuarmeclipse` to mainstream QEMU~~ | ~~Medium~~ | **Partially done**. MPS2-AN386/AN385/AN500/AN505, microbit, and olimex-h405 all work on mainline QEMU. STM32F429 remains on legacy fork for speed. |
| 9 | ~~Replace Eclipse-generated makefiles with CMake or Makefile~~ | ~~Low~~ | Done |
| 10 | ~~Add Cortex-M0+/M3/M7/M33/M55/M85 support~~ | ~~Low~~ | Done. Toolchain supports all 9 targets. QEMU hosts created for M0, M3, M4/M4F, M7, M33. M55/M85 hosts need upstream QEMU board support. |
| 11 | Add unit tests for Python toolchain | Low | Only integration tests via QEMU currently exist |
| 12 | Add Justfile for convenient command running | Low | Done. See `just --list` for available commands. |
| 13 | ~~Add circular dependency support (sentinel + link_dependency)~~ | ~~Medium~~ | Removed in v3.0. Low-level linking primitives (`UDYNLINK_SYM_DEFERRED`, `udynlink_link_symbol()`, etc.) are retained for advanced use cases. |
