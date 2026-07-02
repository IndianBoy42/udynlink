[![CI](../../actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)

# udynlink — Micro Dynamic Linker for ARM Cortex-M

`udynlink` compiles C/C++ code into position-independent binary modules that can be loaded and executed at runtime on ARM Cortex-M MCUs. Modules can run from RAM or flash (execute in place), resolve symbols from the host firmware, and be loaded from contiguous memory or non-contiguous image sources.

## Design Principles

| Principle | What it means in practice |
|-----------|--------------------------|
| **Simplicity** | Minimal API surface: load, call, unload. The core runtime is ~800 lines of C. No DSLs, no code generation, no macro magic beyond what the hardware requires. |
| **Unopinionated** | No imposed module lifecycle, event loop, threading model, or memory strategy. The host decides when and how to load, call, and unload modules. |
| **Usage-agnostic** | RAM bootloaders, flash-resident plugins, OTA patching, scripting language FFI, LGPL compliance — all equally first-class. No use case is privileged. |
| **Flexible** | Three load modes (copy-all, copy-text-data, XIP). Non-contiguous image loading for SD card/SPI flash. Low-level relocation primitives for custom pipelines. Deferred symbols, incremental linking, and direct symbol patching. |
| **Minimal overhead** | No heap allocation inside the loader when the host provides a buffer. No internal locking. No hidden state. The `udynlink_module_t` struct is 24 bytes. |
| **Zero-cost optional features** | The dependency system (`udynlink_deps`), hash-based symbol resolution (`udynlink_hash`), call ergonomics (`udynlink_call`), and host symbol cache (`udynlink_host_utils`) are separate headers linked only if used. Hosts that don't use them pay zero code and zero RAM cost. |
| **Library, not framework** | You call udynlink; udynlink never calls you back except through the five explicit callbacks you implement. No main loop, no registration, no hidden threads. Add it to your build and call the functions you need. |

## Use Cases

- RAM-resident bootloaders and live firmware patching
- Plugin/module systems for embedded applications
- Scripting language FFI and native extension loading
- LGPL-compliant dynamic loading (proprietary host, open modules)
- WASM2C runtime support (load compiled WebAssembly modules)

## Documentation

| Guide | Description |
|-------|-------------|
| [How It Works](docs/how-it-works.md) | Technical deep-dive: PIC model, LOT/r9 mechanism, relocations, binary format |
| [Integrating as a Host](docs/integrating-as-host.md) | Adding udynlink to your project, callbacks, symbol tables, lifecycle |
| [Writing Modules](docs/writing-modules.md) | Creating loadable modules, consuming host symbols, C++ support |
| [API Reference](docs/api-reference.md) | Complete reference for all public functions, structs, macros, and callbacks |
| [Examples](docs/examples.md) | Working code examples for every major feature |
| [Testing Guide](docs/testing.md) | Running tests, adding test cases, adding QEMU platforms |

## Quick Start: Host Firmware (Loading Modules)

**1. Build the library**

```bash
cmake -B build -S .
cmake --build build          # produces build/libudynlink.a
```

**2. Add to your CMake project**

```cmake
add_subdirectory(path/to/udynlink)
target_link_libraries(your_firmware PRIVATE udynlink)
target_compile_definitions(your_firmware PRIVATE
    UDYNLINK_HOST_ARCH_TAG=UDYNLINK_ARCH_TAG_CORTEX_M4
)
```

**3. Implement the required callbacks** (from `udynlink_externals.h`)

```c
#include "udynlink.h"

void *udynlink_external_malloc(size_t size) { return malloc(size); }
void udynlink_external_free(void *p) { free(p); }
void udynlink_external_vprintf(const char *s, va_list va) { vprintf(s, va); }

uintptr_t udynlink_external_resolve_symbol(const udynlink_module_t *p_mod, const char *name) {
    (void)p_mod;
    if (!strcmp(name, "printf")) return (uintptr_t)&printf;
    return 0;
}
int udynlink_external_is_pointer_in_ram(const void *p) {
    return (uintptr_t)p >= 0x20000000 && (uintptr_t)p < 0x20040000;
}
```

**4. Load and call a module**

```c
udynlink_module_t mod;
if (udynlink_load_module(&mod, module_data, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL) != UDYNLINK_OK)
    return;  // handle error

// Prepare r9 for this module before calling any module function
UDYNLINK_PREPARE_CALL(&mod);

void (*hello)(int) = (void (*)(int))udynlink_get_symbol_value(&mod, "hello");
hello(42);

udynlink_unload_module(&mod);
```

For the complete integration guide including hash-based resolution, non-contiguous image loading, deferred symbols, and error handling, see [Integrating as a Host](docs/integrating-as-host.md).

## Quick Start: Module Development

**1. Write your module code**

```c
// mod_hello.c
#include <stdio.h>

int hello(int arg) {         // non-static = exported to host
    printf("Hello %d\n", arg);
    return -arg;
}

static int helper(int x) {   // static = internal, not exported
    return x + 1;
}
```

**2. Compile with mkmodule**

```bash
cd scripts
python3 mkmodule --target cortex-m4 --gen-c-header mod_hello.c
```

This produces `mod_hello.bin` and `mod_hello_module_data.h`.

**Or via CMake:** if your host firmware is CMake-based, consume udynlink via `add_subdirectory`/`FetchContent`/`find_package` and build the module as a target with `udynlink_add_module(mod_hello SOURCES mod_hello.c GENERATE_HEADER)` — see [Building Modules with CMake](docs/writing-modules.md#building-modules-with-cmake).

**3. The host loads and calls your module** (see Quick Start: Host above)

For the complete guide covering C++ modules, data handling, and the full `mkmodule` reference, see [Writing Modules](docs/writing-modules.md).

## Architecture Overview

### Position-Independent Code Model

Modules are compiled with `-fPIE -msingle-pic-base -mno-pic-data-is-text-relative -ffunction-sections -fdata-sections`. Data access uses `r9` as a base register pointing to the **LOT** (Linker Offset Table). Exported functions get an assembly prologue that saves the caller's `r9`, loads `r9` with the module's LOT base, and restores it on return.

The host must use `UDYNLINK_PREPARE_CALL(p_mod)` before calling any module function (or use `UDYNLINK_CALL` which handles this automatically). The `--no-prologue` flag omits the assembly wrapper; the host must set `r9` directly for such modules.

### Three Load Modes

| Mode | RAM usage | Use case |
|------|-----------|----------|
| `COPY_ALL` | Header + code + data + BSS | Maximum flexibility; module can be unloaded from flash |
| `COPY_TEXT_DATA` | Code + data + BSS | Header stays at `base_addr` (e.g., memory-mapped flash) |
| `XIP` | Data + BSS only | Execute code in place from flash; minimal RAM |

All three modes are validated by every test, at both `-O0` and `-Os`.

### Module Binary Format

```
[Header 32B] [Relocations] [Symbol Table] [Code] [Data]
```

The header contains `mod_version`, `udynlink_version`, and `arch_tag` fields for runtime compatibility checking. Relocation types: `R_ARM_GOT_BREL` (LOT), `R_ARM_ABS32`/`R_ARM_TARGET1` (data), `R_ARM_THM_CALL`/`R_ARM_THM_JUMP24` (PC-relative, ignored).

### Optional Layers

All optional features are separate headers that compile and link only if included:

| Header | Provides | Cost when unused |
|--------|----------|-------------------|
| `udynlink_call.h` | `udynlink_func_t`, `UDYNLINK_CALL`, `UDYNLINK_CALL_MODULE_FUNC` | Zero (header-only inline) |
| `udynlink_deps.h` | Cross-module thunks, dependency tracking, circular detection | Zero (separate `.c`, not linked) |
| `udynlink_hash.h` | GNU hash table + bloom filter for O(1) host symbol resolution | Zero (separate `.h`, not linked) |
| `udynlink_host_utils.h` | Tiny host-side symbol cache with LRU eviction | Zero (header-only inline) |
| `udynlink.hpp` | C++ RAII wrappers: `Module`, `Func<Sig>`, `Context` | Zero (header-only, requires C++17) |

## Status

- 29 integration tests across 6 QEMU platforms (Cortex-M0/M3/M4/M4F/M7/M33)
- C and C++ modules supported (no exceptions, no RTTI)
- ABI versioning and architecture tag validation at load time
- Non-contiguous image loading for SD card, SPI flash, and custom pipelines
- Low-level relocation primitives for building custom loading pipelines
- Fine-grained planning APIs (`udynlink_validate_header`, `udynlink_compute_ram_size`)
- Hash-based O(1) symbol resolution
- WASM2C runtime support
- Requires [GCC ARM Embedded](https://developer.arm.com/tools-and-software/open-source-software/developer-tools/gnu-toolchain) (`arm-none-eabi-gcc`)

## Building

### Core library

```bash
cmake -B build -S .
cmake --build build
```

Produces `build/libudynlink.a`. Downstream projects can use `add_subdirectory()` or `find_package(udynlink)` after install.

### Compiling a loadable module

```bash
cd scripts
python3 mkmodule --target cortex-m4 source.c
python3 mkmodule --target cortex-m33 --public-symbols init,start,stop source.c
```

Supported targets: `cortex-m0`, `cortex-m0plus`, `cortex-m3`, `cortex-m4`, `cortex-m4f`, `cortex-m7`, `cortex-m33`, `cortex-m55`, `cortex-m85`.

See [Writing Modules](docs/writing-modules.md#the-mkmodule-command-reference) for the full CLI reference.

## Testing

Tests run under QEMU via `just`. Do not invoke `test_driver.py` manually with ad-hoc environment variables.

```bash
just test-mps2           # MPS2-AN386 (Cortex-M4, mainline QEMU)
just test-an385           # MPS2-AN385 (Cortex-M3)
just test-an500           # MPS2-AN500 (Cortex-M7)
just test-an505           # MPS2-AN505 (Cortex-M33)
just test-h405            # Olimex STM32-H405 (Cortex-M4F hard-float)
just test-f429            # STM32F429 (legacy xPack QEMU, fast)
just ci                   # Full CI suite (parallel)
just test-mps2-single test-globals1   # Single test
```

Each test validates all three load modes at both `-O0` and `-Os`. See the [Testing Guide](docs/testing.md) for adding test cases, adding platforms, and debugging.

## Platform Test Matrix

| Platform | QEMU Machine | CPU | Status |
|----------|-------------|-----|--------|
| `stm32f429_discovery` | STM32F429I-Discovery | cortex-m4 | All tests pass (legacy xPack QEMU) |
| `mps2_an386` | mps2-an386 | cortex-m4 | All tests pass |
| `mps2_an385` | mps2-an385 | cortex-m3 | All tests pass |
| `mps2_an500` | mps2-an500 | cortex-m7 | All tests pass |
| `mps2_an505` | mps2-an505 | cortex-m33 | All tests pass |
| `olimex_stm32_h405` | olimex-stm32-h405 | cortex-m4f | All tests pass |
| `microbit` | microbit | cortex-m0 | Builds only (QEMU `-kernel` limitation) |
| `stm32f103_bluepill` | NUCLEO-F103RB | cortex-m3 | Partial (legacy QEMU Flash→RAM quirk) |
| `stm32f051_discovery` | STM32F0-Discovery | cortex-m0 | Partial (same quirk) |

## Toolchain Requirements

- **arm-none-eabi-gcc** / **arm-none-eabi-g++** / **arm-none-eabi-objcopy** (GCC ARM Embedded)
- **CMake** 3.16+
- **Python 3** with `pyelftools`, `Jinja2` (managed via `uv` / `pyproject.toml`)
- **QEMU** for tests (see setup below)
- **[just](https://github.com/casey/just)** for running tests and build commands

### QEMU Setup

Two QEMU variants are used:

- **Mainline QEMU** (`qemu-system-arm` 9.2.4+): MPS2 and Olimex platforms. Usually available via your distro package manager.
- **Legacy xPack QEMU** (`qemu-system-gnuarmeclipse`): Fastest for STM32F429. Discontinued in recent xPack releases.

**Quick setup:**
- `just setup-qemu` — download latest xPack release (mainline `qemu-system-arm`) into `tests/`.
- `just setup-qemu-legacy` — download the last release with `qemu-system-gnuarmeclipse` (7.2.5-1) into `tests/`.

The Justfile automatically prefers these local copies over system-wide installations.

## License

Apache 2.0
