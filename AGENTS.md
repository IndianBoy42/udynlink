## What This Repo Is

`udynlink` is a **micro dynamic linker for ARM Cortex-M MCUs**. It loads position-independent C/C++ binary modules at runtime on embedded targets. The core runtime is ~800 lines of C; the build toolchain is a set of Python 3 scripts that compile C/C++ to a custom loadable module format.

## Design Principles

These principles govern all development decisions. When in doubt, refer back to them:

| Principle | Practical meaning |
|-----------|-------------------|
| **Simplicity** | Minimal API surface. No DSLs, no code generation, no macro magic beyond what the hardware requires. New features must justify their complexity. |
| **Unopinionated** | No imposed lifecycle, event loop, threading model, or memory strategy. The host decides when and how to load, call, and unload. |
| **Usage-agnostic** | Bootloaders, plugins, OTA patching, scripting FFI, LGPL compliance — all equally first-class. No use case is privileged in the API. |
| **Flexible** | Three load modes, non-contiguous image loading, low-level relocation primitives, deferred symbols, incremental linking, direct symbol patching. The host can build any pipeline on top. |
| **Minimal overhead** | No heap allocation when the host provides a buffer. No internal locking. No hidden state. `udynlink_module_t` is 24 bytes. |
| **Zero-cost optional features** | `udynlink_deps`, `udynlink_thunk`, `udynlink_hash`, `udynlink_call`, `udynlink_host_utils` — separate headers linked only if used. Unused optional features compile to zero code and zero RAM. |
| **Library, not framework** | You call udynlink; udynlink never calls you back except through the five explicit callbacks you implement. No main loop, no registration, no hidden threads. |

### Checking a change against the principles

Before adding a feature or changing behavior, ask:
1. Does it add an opinion the host didn't ask for? If yes, make it optional.
2. Does it cost anything when not used? If yes, put it in a separate compilation unit.
3. Does it require the user to restructure their code? If yes, provide a simpler alternative.
4. Can the host achieve the same result by composing existing lower-level primitives? If yes, prefer that over adding a new API.

## Documentation Structure

All user-facing documentation lives under `docs/` and is summarized in `docs/README.md`:

| Guide | Description |
|-------|-------------|
| `docs/how-it-works.md` | Technical deep-dive: PIC model, LOT/r9 mechanism, relocations, binary format, ABI versioning |
| `docs/integrating-as-host.md` | Adding udynlink to your firmware, implementing callbacks, symbol tables, lifecycle, thread safety |
|`docs/writing-modules.md`|Creating loadable C/C++ modules, consuming symbols, mkmodule reference|
|`docs/protobuf-modules.md`|Compiling `.proto` definitions into parse/write UDLM modules (`scripts/proto2module`), 1-module-per-struct overhead analysis|
|`docs/wasm2c-modules.md`|Compiling `.wasm`/`.wat` into UDLM modules (`scripts/mkwasm2c-module`); memory models, trap policy, symbol-table policy, testing|
| `docs/api-reference.md` | Complete reference for all public functions, structs, macros, and callbacks |
| `docs/examples.md` | Working code examples for every major feature |
| `docs/testing.md` | Running tests, adding test cases and platforms, debugging **MUST READ before testing** |
| `docs/host-testing.md` | Testing module logic on the host machine without QEMU/ARM tools; mocking patterns, CMake helper, and vendored template |
| `docs/thread-safety.md` | Modules in multithreaded hosts (FreeRTOS/Zephyr): execution vs lifecycle, r9 preemption safety, same-module multi-thread rules, nested calls |
| `docs/fuzzing.md` | Host sanitizer (ASan+UBSan) and libFuzzer harnesses that exercise the udynlink loader itself natively; seed corpus, crash triage |

> **Always keep documentation in sync.** If you change code, public APIs, test behavior, build commands, or toolchain requirements, update the corresponding `docs/*.md` file(s) before finishing the task. `AGENTS.md` itself must also be updated if build/test commands, architecture constraints, or the platform matrix change.

## Toolchain Requirements

- **`arm-none-eabi-gcc`** / **`arm-none-eabi-g++`** / **`arm-none-eabi-objcopy`** (GCC ARM Embedded)
- **CMake** ≥ 3.16
- **Python 3** with `pyelftools`, `Jinja2` (managed via `uv` / `pyproject.toml`)
- **QEMU** for tests (two variants):
  - **Mainline QEMU** (`qemu-system-arm` 9.2.4+): MPS2 and Olimex platforms.
  - **Legacy xPack QEMU** (`qemu-system-gnuarmeclipse`): Fastest for STM32F429. Discontinued in recent xPack releases.
  - Quick setup: `just setup-qemu` (mainline) and `just setup-qemu-legacy` (legacy) — downloads into `tests/`. The Justfile prefers local copies over system-wide installations.
  - Override via `UDYNLINK_QEMU_BIN` and `UDYNLINK_QEMU_LEGACY_BIN` environment variables.
- **[just](https://github.com/casey/just)** for running tests and build commands
- **wabt 1.0.34** (`wasm2c` + `wat2wasm`) for wasm module builds: `just setup-wabt` downloads a
  checksum-pinned release into `tools/wabt/`; `scripts/mkwasm2c-module` prefers it over PATH and
  warns when the installed wasm2c is older than the tested minimum (`UDYNLINK_WASM2C` overrides).

**Optional tools:**
- **`scripts/mkhostsyms`** — reads a host firmware ELF and generates a C header with a const GNU hash table (`--format gnu-hash`, default) or search trie (`--format trie`) for O(1)/O(k) symbol resolution
- **`scripts/proto2module`** — compiles `.proto` files into protobuf codec modules (parse/write per message) via protoc + nanopb. Requires `protoc` and the nanopb generator plugin (`uv pip install nanopb`); the nanopb C runtime is vendored at `third_party/nanopb` (see `docs/protobuf-modules.md`)
- **`scripts/mkwasm2c-module`** — compiles `.wasm`/`.wat` into UDLM modules via wasm2c + mkmodule
  (bare-metal wasm runtime in `udynlink/wasm2c_runtime/`). Memory models (`--memory=static|dynamic|external`),
  `--custom-page-size`, `--stack-depth-limit`, `--trap-handler`, optional trap containment
  (`--recoverable-traps` / `--wrappers-recover`), imports via the symbol contract
  (`--gen-imports-header`), symbol-table policy. See `docs/wasm2c-modules.md`

## Build & Test Commands

### Build the core library
```bash
cmake -B build -S .
cmake --build build
```
Produces `build/libudynlink.a` and install targets for headers. Downstream projects can consume via `add_subdirectory()` or `find_package(udynlink)` after install.

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
- `-I <dir>` / `--include-dir <dir>` — add a directory to the module compile include path (repeatable), e.g. `-I<repo>/udynlink` so module sources can `#include "udynlink_deps_api.h"` instead of pasting the dependency macros inline
- `--mcpu <cpu>` — target CPU (default: `cortex-m4`)
- `--target <name>` — target from the target database (default: `cortex-m4`). Supported: `cortex-m0`, `cortex-m0plus`, `cortex-m3`, `cortex-m4`, `cortex-m4f`, `cortex-m7`, `cortex-m33`, `cortex-m55`, `cortex-m85`
- `--mod-version <ver>` — module ABI version (default: `1.0`)
- `--udynlink-version <ver>` — loader ABI version (default: `3.0`)
- `--no-prologue` — omit assembly prologue wrappers; sets `UDYNLINK_ARCH_FLAG_NO_PROLOGUE` in the module header
- `--lto` — link-time optimization (GCC `-flto`, fat objects, `--wrap`-based prologues; see `docs/writing-modules.md` → LTO Mode)
- `--workdir <dir>` — directory for intermediate files (`*.o`, `*.elf`, `*.s`) and the default `.bin` output, keeping the source tree clean (default: next to source; also via `UDYNLINK_WORKDIR` env var)

For C++ sources (`.cpp`/`.cxx`), the toolchain automatically adds `-fno-exceptions -fno-rtti -fno-use-cxa-atexit` and compiles `cpp_init_fini.c` for `__init_array` support.

The compiler prefix can be overridden via the `UDYNLINK_CC_PREFIX` environment variable (default: `arm-none-eabi-`).

**Building via CMake (downstream projects):** a host firmware that consumes udynlink via `add_subdirectory`/`FetchContent`/`find_package` can build a module as a CMake target with `udynlink_add_module(<name> SOURCES ... GENERATE_HEADER)`. It produces a custom target `<name>` (→ `<name>.bin` in the build tree) and an `udynlink::module::<name>` INTERFACE library a firmware target links to consume the generated `*_module_data.h` with correct rebuild ordering. The helper (`cmake/udynlinkAddModule.cmake`) is installed alongside `udynlinkGenerateHostSyms.cmake`. See `docs/writing-modules.md` → "Building Modules with CMake".

### Build a WebAssembly module
```bash
python3 scripts/mkwasm2c-module --gen-c-header --header-path /some/path module.wat
```

Accepts `.wat` or `.wasm`. Key flags (full reference: `docs/wasm2c-modules.md`):
- `--memory=static|dynamic|external` — linear-memory model (default `auto`: static unless the module uses `memory.grow`)
- `--gen-imports-header` — emit `<bin>_imports.h`, the host-side symbol contract (auto with `--gen-c-header` when the module has imports); host implements the listed wasm2c-named functions and resolves them via `udynlink_external_resolve_symbol`
- `--custom-page-size=N` — shrink the 64 KiB wasm page
- `--stack-depth-limit=N` — wasm recursion traps (`WASM_RT_TRAP_EXHAUSTION`) instead of native stack overflow
- `--trap-handler=NAME` — host-provided trap handler symbol (resolved at load)
- `--recoverable-traps` / `--wrappers-recover` — optional trap containment: host-registered one-shot `longjmp` recovery point (exported `wasm_rt_set_recovery`/`wasm_rt_last_trap`) or baked wrapper sentinels; fatal `bkpt` loop stays the zero-cost default
- `--malloc=NAME` / `--free=NAME` — allocator hook overrides
- `--export-all` — export every symbol (default: export wrappers only, minimal symtab)
- `--public-symbols a,b` / `--wrapper-prefix PFX` / `--no-export-wrappers` — symbol-table control
- `--workdir <dir>` / `--keep` — keep intermediates for debugging

The wasm runtime lives in `udynlink/wasm2c_runtime/` (`wasm-rt.h`, `wasm-rt-udynlink.c`) — the single
source of truth; the script copies it into the build, and per-module settings are generated into
`wasm_rt_config.h` (picked up by every TU via `__has_include`). Test modules (`tests/test-wasm2c-*`)
are built through the script by `test_driver.py` (a test dir carries `<name>.wat` and a
`"wasm": "<name>.wat"` entry in `test_data.py`).

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

# Python tests
just test-py                   # Python unit tests only (no ARM toolchain needed)
just test-py-all               # All Python tests including integration (needs arm-none-eabi-gcc)

# Host sanitizer & fuzz testing (loader, not module logic; see docs/fuzzing.md)
just fuzz-seeds        # Regenerate tests/fuzz/corpus/*.bin from in-repo module sources
just test-san          # Build and run the ASan+UBSan loader regression gate (gcc or clang)
just fuzz              # Build and run the libFuzzer harness for 60 seconds (requires clang)
just fuzz 10           # Run the libFuzzer harness for 10 seconds
```

The fuzz/san targets are gated behind the `UDYNLINK_BUILD_FUZZERS` CMake option
(OFF by default). `just fuzz` requires `clang` for `-fsanitize=fuzzer`; the
sanitizer gate (`just test-san`) builds under gcc or clang.

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

## Public Headers

| Header | Layer | Provides | When to use |
|--------|-------|----------|-------------|
| `udynlink.h` | Core (required) | Load, unload, symbol lookup, validation, linking primitives | Always |
| `udynlink_externals.h` | Core (required) | `udynlink_external_malloc`, `udynlink_external_free`, `udynlink_external_vprintf`, `udynlink_external_resolve_symbol`, `udynlink_external_is_pointer_in_ram` | Always (host must implement) |
| `udynlink_call.h` | Optional (inline) | `udynlink_func_t`, `udynlink_resolve_func()`, `UDYNLINK_CALL`, `UDYNLINK_CALL_MODULE_FUNC` | Convenient r9 save/restore around module calls |
|`udynlink_deps.h`|Optional (separate .c)|Cross-module thunks, dependency tracking, circular detection, `UDYNLINK_REQUIRES`, `UDYNLINK_THUNK_EXPORT`|Modules that call other modules|
|`udynlink_deps_api.h`|Optional (inline, module-facing)|`UDYNLINK_REQUIRES`, `UDYNLINK_THUNK_GATEWAY`, `UDYNLINK_THUNK_EXPORT` — self-contained, no host API|Module sources declaring deps / preallocated thunk exports (reached via `mkmodule -I`)|
|`udynlink_thunk.h`|Optional (separate .c)|Thunk pool, gateway/stub allocation, `udynlink_thunk_make_call()`, `udynlink_external_find_stub()`|Creating callable function pointers for module symbols without r9 management|
| `udynlink_hash.h` | Optional (inline) | GNU hash table + bloom filter for O(1) host symbol resolution | Hosts exporting many symbols |
| `udynlink_trie.h` | Optional (inline) | Compact search trie for O(k) host symbol resolution | Hosts wanting prefix-sharing or no Bloom overhead |
| `udynlink_host_utils.h` | Optional (inline) | Tiny host-side symbol cache with LRU eviction | Speeding up repeated `udynlink_external_resolve_symbol` calls |
| `udynlink_cpp_abi.h` | Optional (inline) | Weak stubs + resolver for the C++ ABI symbols (`operator delete`, `__cxa_pure_virtual`, ...) referenced by loadable C++ modules | C-only bare-metal hosts loading C++ modules with virtual destructors, abstract classes, or `new`/`delete` |
| `udynlink.hpp` | Optional (C++23 inline) | `Module` (RAII lifecycle), `Func<Sig>` (typed function handle), `Context` (RAII r9 manager), `Symbol`/`SymbolView` (C++23 range over a module's symbol table, pre- or post-load) | C++ hosts wanting type safety and automatic cleanup |
| `udynlink/wasm2c_runtime/wasm-rt.h` | Wasm runtime (wasm modules only) | wasm2c runtime types, trap codes, memory/table API, weak host hooks; per-module config via generated `wasm_rt_config.h` | Compiled into wasm modules by `scripts/mkwasm2c-module` |
| `udynlink/wasm2c_runtime/wasm-rt-udynlink.c` | Wasm runtime (wasm modules only) | Bare-metal wasm2c runtime: static/dynamic/external linear memory, zeroed tables, trap dispatch, wasm call-depth counting | Same |

## Architecture & Key Constraints

### Position-Independent Code Model
- Modules are compiled with `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections`
- Data access uses `r9` as a base register pointing to the **LOT** (Linker Offset Table)
- Exported functions get an assembly prologue (generated from `scripts/asm_template_*.tmpl`) that saves the caller's `r9`, loads `r9` with the module's LOT base (`p_mod->ram_base`), and restores the original `r9` on return
- **Host must use `UDYNLINK_PREPARE_CALL(p_mod)` before calling any module function** (or use `UDYNLINK_CALL` which handles this automatically)
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

**Important limitation:** direct internal calls (`bl`) are PC-relative and resolved at link time. The generated prologue wrapper unconditionally branches to the module's renamed local implementation, so **internal callers always use the module's own weak definition**. Host override is only effective for:
- **Data weak symbols** — LOT/data relocations are patched at load time
- **Function pointers / indirect calls** — address taken through the GOT
- **External callers** — `udynlink_lookup_symbol()` resolves the override at runtime

Undefined weak symbols (`STB_WEAK` + `SHN_UNDEF`) are treated as `external` and still fail module load if unresolved. Standard ELF silently resolves them to NULL; a dedicated `weak_undef` type could be added in a follow-up.

### C++ Module Support
- Call `udynlink_cpp_init(p_mod)` after loading a C++ module to run global constructors via `__init_array`
- The host must set `r9` to the module's LOT base (`p_mod->ram_base`) before calling `udynlink_cpp_init`, using `UDYNLINK_PREPARE_CALL(p_mod)` or `UDYNLINK_CALL`
- Heavily templated C++ modules can ship symbol tables larger than their code+data. `mkmodule` offers four opt-in bloat-reduction flags (`--strip-hidden-syms`, `--strip-non-public-syms`, `--strip-mangled-syms`, `--strip-weak-sym-names`) that demote defined symbols to nameless internal entries without changing loader or binary format. See `docs/writing-modules.md` → "Controlling C++ Symbol-Table Size".

### Module Image Format
Binary modules start with the signature `UDLM`, followed by a 32-byte header, relocation table, symbol table, `.text`, and `.data`. The loader (`udynlink_load_module`) validates the signature, checks ABI version, applies relocations, and resolves extern symbols.

Binary layout: [Header 32B] [Relocs] [Symtab] [Code] [Data]

Relocation types handled: `R_ARM_GOT_BREL` (LOT), `R_ARM_ABS32` and `R_ARM_TARGET1` (data), `R_ARM_THM_CALL`/`R_ARM_THM_JUMP24` (ignored, PC-relative).

The header contains `mod_version`, `udynlink_version`, and `arch_tag` fields for runtime compatibility checking. `arch_tag` encodes the core family, FPU presence, and float ABI.

### Three Load Modes
All tests validate all three modes by default:
- `UDYNLINK_LOAD_MODE_COPY_ALL`: copy header + text + data to RAM
- `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA`: copy text + data to RAM (header stays at base_addr)
- `UDYNLINK_LOAD_MODE_XIP`: copy only data to RAM; execute code in place from flash

### Dependency System (`udynlink_deps`)
The optional `udynlink/udynlink_deps.h` layer provides cross-module function calls via runtime-generated RAM thunks. The thunk pool and gateway/stub allocation are implemented in the `udynlink_thunk` layer, which `udynlink_deps` includes and delegates to. The thunk mechanism can also be used independently of the dependency system via `udynlink_thunk_make_call()` to create callable function pointers for module symbols without r9 management. Two-level dispatch: per-module gateways (18 bytes) and per-function stubs (10 bytes). Stubs load the target address into `r12` (IP) via `movw+movt`, then branch to the module's shared gateway which switches `r9` and calls the function. This preserves `r0-r3` argument registers.

- Modules declare dependencies with `UDYNLINK_REQUIRES(mod_name)` which emits a `.udynlink.mod.requires.{name}` symbol.
- The host's `udynlink_external_resolve_symbol()` checks `udynlink_dep_is_dependency()` first, then `udynlink_dep_resolve_func()` for functions, then `udynlink_dep_resolve_data()` for data, and finally falls back to host-native symbols.
- `udynlink_dep_load()` and `udynlink_dep_unload()` wrap the core loader with automatic module registration and circular-dependency detection (max depth 8).

### Module Uniqueness
Multiple instances of the same module are allowed (no deduplication by name).

## Testing Platform Matrix

| Platform | QEMU Machine | QEMU Binary | CPU | Status | Notes |
|----------|--------------|-------------|-----|--------|-------|
| `stm32f429_discovery` | STM32F429I-Discovery | `qemu-system-gnuarmeclipse` | cortex-m4 | All tests pass | Legacy xPack fork; `test-strip-init-array` skipped due to semihosting heap corruption |
| `mps2_an386` | mps2-an386 | `qemu-system-arm` (9.2.4+) | cortex-m4 | All tests pass | Mainline QEMU, ~0.5s/test |
| `olimex_stm32_h405` | olimex-stm32-h405 | `qemu-system-arm` | cortex-m4f | All tests pass | Hard-float M4F on mainline QEMU |
| `mps2_an385` | mps2-an385 | `qemu-system-arm` | cortex-m3 | All tests pass | Mainline QEMU |
| `mps2_an500` | mps2-an500 | `qemu-system-arm` | cortex-m7 | All tests pass | Mainline QEMU |
| `mps2_an505` | mps2-an505 | `qemu-system-arm` | cortex-m33 | All tests pass | Mainline QEMU, secure boot alias at 0x10000000 |
| `microbit` | microbit | `qemu-system-arm` | cortex-m0 | Builds only | QEMU does not support ELF `-kernel` at 0x00000000 |
| `stm32f103_bluepill` | NUCLEO-F103RB | `qemu-system-gnuarmeclipse` | cortex-m3 | Partial | Flash→RAM host call quirk (legacy QEMU) |
| `stm32f051_discovery` | STM32F0-Discovery | `qemu-system-gnuarmeclipse` | cortex-m0 | Partial | Same quirk as M3 |

**Dual-QEMU Strategy:**
- **STM32F429** (legacy xPack `qemu-system-gnuarmeclipse`): Fast baseline/regression testing
- **MPS2-AN386** (mainline `qemu-system-arm` 9.2.4+): Future-proof, validates no xPack-specific bugs
- All other platforms target mainline QEMU for future compatibility

**MPS2-AN505 (Cortex-M33) Note:**
QEMU boots the Cortex-M33 in **Secure state** and fetches the initial vector table from the secure alias address `0x10000000`. The `tests/platforms/mps2_an505/mem.ld` linker script places the vector table at `0x10000000` so `-kernel` loading works directly.

## Known Issues

- **No thread safety** — the loader uses no locks or atomics. Concurrent load/unload from different interrupt levels will corrupt state. The host must provide synchronization. (OUT OF SCOPE for the library itself — the host owns the threading model.)
- **M3/M0 QEMU hosts have Flash→RAM call quirk** — Modules calling host functions (e.g. `printf`) hang under `qemu-system-gnuarmeclipse` for STM32F103/STM32F051 boards, but work correctly on STM32F429. This is a known `qemu-system-gnuarmeclipse` emulation bug; mainline QEMU (`qemu-system-arm`) does **not** exhibit this issue.
- **F429 semihosting heap corruption** — `qemu-system-gnuarmeclipse` 2.8.0 corrupts heap-allocated guest RAM at low addresses (0x20000400–0x20000600 range) during semihosting `SYS_WRITE0` calls. This causes `test-strip-init-array` to fail on the F429 platform. All other platforms using mainline `qemu-system-arm` are unaffected. The test is skipped on `stm32f429_discovery` via `skip_platforms` in `test_data.py`.
- **xPack QEMU 9.2.4 discontinued `qemu-system-gnuarmeclipse`** — Latest xPack releases only include `qemu-system-arm` (mainline). STM32F429 fast testing requires an older xPack release or the `xpack-dev-tools/qemu-arm` project.

## Generated / Ignored Files

The `.gitignore` and test harness generate these artifacts; do not commit them:
- `*.o`, `*.elf`, `*.bin`, `*.hex`, `*.map`, `*.d`, `*.pyc`
- `*_module_data.h` (generated by `mkmodule --gen-c-header`)
- `tests/qemu_host/src/test_qemu.c` (copied by test driver from the active test case)
- `temp/*`

## CI

`.github/workflows/ci.yml` runs the test suite via GitHub Actions. It installs `gcc-arm-embedded`, CMake, Python 3 deps, and QEMU.

## Open TODOs

| # | Task | Priority | Notes |
|---|------|----------|-------|
| 1 | Add unit tests for mkmodule | Low | mkhostsyms now has tests; mkmodule still only tested via QEMU |
| 2 | Migrate STM32F429 tests to mainline QEMU | Medium | Depends on suitable MPS2-level board support in mainline QEMU for M4 |
