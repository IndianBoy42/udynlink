[![CI](../../actions/workflows/ci.yml/badge.svg)](../../actions/workflows/ci.yml)

# udynlink — Micro Dynamic Linker for ARM Cortex-M

`udynlink` compiles C/C++ code into position-independent binary modules that can be loaded and executed at runtime on ARM Cortex-M MCUs. Modules can run from RAM or flash (execute in place), resolve symbols from the host firmware and from each other, and be loaded from memory or streaming I/O sources.

**Use cases:** RAM-resident bootloaders, runtime firmware patching, plugin/module systems, scripting language loaders, LGPL-compliant dynamic loading.

## Documentation

| Guide | Audience | Description |
|-------|----------|-------------|
| [How It Works](docs/how-it-works.md) | Everyone | Technical deep-dive: PIC model, LOT/r9 mechanism, relocations, binary format |
| [Integrating as a Host](docs/integrating-as-host.md) | Firmware developers | Adding udynlink to your project, implementing callbacks, symbol tables, lifecycle |
| [Writing Modules](docs/writing-modules.md) | Module authors | Creating loadable modules, consuming host symbols, dependencies, C++ support |
| [API Reference](docs/api-reference.md) | Everyone | Complete reference for all public functions, structs, macros, and callbacks |
| [Examples](docs/examples.md) | Everyone | Working code examples for every major feature and use case |
| [Testing Guide](docs/testing.md) | Contributors | Running tests, adding test cases, adding QEMU platforms, debugging |

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

uint32_t udynlink_external_resolve_symbol(const char *name) {
    if (!strcmp(name, "printf")) return (uint32_t)(uintptr_t)&printf;
    return 0;
}
uint32_t udynlink_external_resolve_critical_symbol(const char *name) { return 0; }
int udynlink_external_is_pointer_in_ram(const void *p) {
    return (uintptr_t)p >= 0x20000000 && (uintptr_t)p < 0x20040000;
}
udynlink_module_t *udynlink_external_get_module_handle(const char *name) { return NULL; }
```

**4. Load and call a module**

```c
udynlink_module_t mod;
if (udynlink_load_module(&mod, module_data, NULL, 0, UDYNLINK_LOAD_MODE_COPY_ALL) != UDYNLINK_OK)
    return;  // handle error

// Critical: set LOT base before calling any module function
*(uint32_t *)UDYNLINK_LOT_BASE_ADDR = mod.ram_base;

void (*hello)(int) = (void (*)(int))udynlink_get_symbol_value(&mod, "hello");
hello(42);

udynlink_unload_module(&mod);
```

See the [Host Integration Guide](docs/integrating-as-host.md) for complete details on callbacks, symbol tables, hash-based resolution, streaming I/O, and error handling.

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

**3. The host loads and calls your module** (see Quick Start: Host above)

For the complete guide covering dependencies, C++ modules, data handling, and the full `mkmodule` reference, see [Writing Modules](docs/writing-modules.md).

## Status

- All 24 integration tests pass on 6 QEMU platforms (Cortex-M0/M3/M4/M4F/M7/M33)
- C and C++ modules supported (no exceptions, no RTTI)
- ABI versioning and architecture tag validation at load time
- Module dependency tracking with safe unload
- Streaming I/O loading (SD card, SPI flash, network)
- Hash-based O(1) symbol resolution
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
python3 mkmodule --target cortex-m33 --depends mod_a,mod_b source.cpp
python3 mkmodule --target cortex-m7 --public-symbols init,start,stop source.c
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

Each test validates all three load modes (COPY_ALL, COPY_TEXT_DATA, XIP) at both `-O0` and `-Os`. See the [Testing Guide](docs/testing.md) for adding test cases, adding platforms, and debugging.

## Platform Test Matrix

| Platform | QEMU Machine | CPU | Status | Notes |
|----------|-------------|-----|--------|-------|
| `stm32f429_discovery` | STM32F429I-Discovery | cortex-m4 | All 24 tests pass | Legacy xPack QEMU, fast baseline |
| `mps2_an386` | mps2-an386 | cortex-m4 | All 24 tests pass | Mainline QEMU |
| `mps2_an385` | mps2-an385 | cortex-m3 | All 24 tests pass | Mainline QEMU |
| `mps2_an500` | mps2-an500 | cortex-m7 | All 24 tests pass | Mainline QEMU |
| `mps2_an505` | mps2-an505 | cortex-m33 | All 24 tests pass | Mainline QEMU, secure boot alias |
| `olimex_stm32_h405` | olimex-stm32-h405 | cortex-m4f | All 24 tests pass | Hard-float M4F |
| `microbit` | microbit | cortex-m0 | Builds, `-kernel` broken | Needs raw binary loader |
| `stm32f103_bluepill` | NUCLEO-F103RB | cortex-m3 | Partial | Flash-RAM call quirk (legacy QEMU) |
| `stm32f051_discovery` | STM32F0-Discovery | cortex-m0 | Partial | Same quirk |

## Toolchain Requirements

- **arm-none-eabi-gcc** / **arm-none-eabi-g++** / **arm-none-eabi-objcopy**
- **CMake** 3.16+
- **Python 3** with `pyelftools`, `Jinja2` (managed via `uv` / `pyproject.toml`)
- **QEMU** for tests: `qemu-system-arm` 9.2.4+ or legacy `qemu-system-gnuarmeclipse`
- **[just](https://github.com/casey/just)** for running tests and build commands

## License

Apache 2.0
