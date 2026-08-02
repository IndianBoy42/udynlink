# Testing Guide

- [Testing Architecture Overview](#testing-architecture-overview)
- [How to Run Tests](#how-to-run-tests)
- [QEMU Setup](#qemu-setup)
- [Platform Test Matrix](#platform-test-matrix)
- [How to Add a New Test Case](#how-to-add-a-new-test-case)
- [How to Add a New QEMU Platform](#how-to-add-a-new-qemu-platform)
- [Debugging Failing Tests](#debugging-failing-tests)
- [Test Driver Internals](#test-driver-internals)

## Testing Architecture Overview

udynlink uses **QEMU-based integration tests** to validate the full pipeline from C/C++ source to a running loadable module. The test flow for every case is:

1. **Compile module sources** with `scripts/mkmodule` to produce a `.bin` and a generated C header (`*_module_data.h`).
2. **Build the QEMU host firmware** (`test1.elf`) that links the core udynlink library, the test harness, and the generated module data.
3. **Run the ELF under QEMU** and capture semihosted stdout.
4. **Validate the output** against golden markers and regexes.

### Golden Output Matching

A test passes only if QEMU output contains the string `*** TEST OK ***`. In addition, every regex in `test_data["required"]` must match the output at least `total_loads` times (default: once per load mode). This catches regressions in both the loader and the module runtime behavior.

### Load Mode Coverage

Inside each `test_qemu.c`, the harness iterates over all three load modes:

- `UDYNLINK_LOAD_MODE_COPY_ALL` — copy header, text, and data to RAM.
- `UDYNLINK_LOAD_MODE_COPY_TEXT_DATA` — copy text and data to RAM; leave header at `base_addr`.
- `UDYNLINK_LOAD_MODE_XIP` — copy only data to RAM; execute code in place from flash.

The loop iterates over `UDYNLINK_LOAD_MODE_COPY_ALL` through `UDYNLINK_LOAD_MODE_XIP`, so adding a new load mode in the future will automatically be exercised by every existing test.

### Optimization Coverage

The test driver (`test_driver.py`) runs each test directory **twice**:

- once with `-O 3`
- once with `-Os` (default `mkmodule` optimization)

Because the harness internally exercises all three load modes, **each test runs 6 times by default** (3 load modes x 2 optimization levels). See [Module Guide](writing-modules.md) for how compiler flags affect generated code. The test suite contains 32 test directories; each directory is compiled and run twice (O3 and Os), yielding 64 top-level test runs on platforms where all tests are enabled.

## How to Run Tests

**Always use `just`.** The `Justfile` encodes the correct QEMU flags, module targets, and timeouts for every platform. Running `test_driver.py` manually with ad-hoc environment variables is not supported and will likely fail.

### Available Test Suites

| Command | Platform | QEMU | CPU | Notes |
|---------|----------|------|-----|-------|
| `just test-mps2` | MPS2-AN386 | `qemu-system-arm` | Cortex-M4 | Mainline QEMU, recommended |
| `just test-an385` | MPS2-AN385 | `qemu-system-arm` | Cortex-M3 | Mainline QEMU |
| `just test-an500` | MPS2-AN500 | `qemu-system-arm` | Cortex-M7 | Mainline QEMU |
| `just test-an505` | MPS2-AN505 | `qemu-system-arm` | Cortex-M33 | Mainline QEMU |
| `just test-h405` | Olimex STM32-H405 | `qemu-system-arm` | Cortex-M4F hard-float | Mainline QEMU |
| `just test-f429` | STM32F429I-Discovery | `qemu-system-gnuarmeclipse` | Cortex-M4 | Legacy xPack QEMU, fast baseline |
| `just test-f103` | STM32F103 Blue Pill | `qemu-system-gnuarmeclipse` | Cortex-M3 | Legacy QEMU, partially working |
| `just test-f051` | STM32F051 Discovery | `qemu-system-gnuarmeclipse` | Cortex-M0 | Legacy QEMU, partially working |
| `just test-microbit` | BBC micro:bit | `qemu-system-arm` | Cortex-M0 | Broken: `-kernel` not supported |

### Running a Single Test

```bash
just test-mps2-single test-globals1
```

This compiles and runs only `tests/test-globals1/` on MPS2-AN386, still exercising all 6 mode/opt combinations.

### Full CI Suite

```bash
just ci
```

This runs the five fully-passing mainline QEMU platforms in parallel: MPS2-AN386, AN385, AN500, AN505, and Olimex H405.

### Environment Variables

The following variables are read by `test_driver.py` and the `Justfile`:

| Variable | Purpose | Example |
|----------|---------|---------|
| `UDYNLINK_QEMU_BIN` | Path to QEMU binary | `qemu-system-arm` |
| `UDYNLINK_QEMU_MACHINE` | QEMU `-machine` or `-board` name | `mps2-an386` |
| `UDYNLINK_QEMU_CPU` | QEMU `-cpu` flag | `cortex-m4` |
| `UDYNLINK_QEMU_EXTRA_FLAGS` | Additional QEMU arguments | `-semihosting` |
| `UDYNLINK_QEMU_TIMEOUT` | Seconds before QEMU is killed (default: 5) | `30` |
| `UDYNLINK_MODULE_TARGET` | Target CPU passed to `mkmodule --target` | `cortex-m3` |
| `UDYNLINK_PLATFORM` | Platform directory name under `tests/platforms/` | `mps2_an386` |
| `UDYNLINK_TEST_DEBUG` | If set, enables `UDYNLINK_DEBUG_INFO` in the test build | `1` |
| `UDYNLINK_TEST_CLEAN` | If set, deletes the CMake build dir before building | `1` |


### Host sanitizer & fuzz testing

Independently of QEMU, the loader itself (`udynlink/udynlink.c`) is exercised
natively on the host by an opt-in ASan+UBSan regression gate and a libFuzzer
harness under `tests/fuzz/`. Both compile the loader with the host compiler
(never `arm-none-eabi-*`), feed it real `mkmodule`-generated `.bin` images and
mutations thereof, and treat any ASan/UBSan report or signal as a failure.
See [Host Sanitizer & Fuzz Testing](fuzzing.md) for build/run recipes
(`just test-san`, `just fuzz`, `just fuzz-seeds`), the trust model being
tested, and how to read and minimize a crash.
## QEMU Setup

### Quick Setup: xPack QEMU (recommended)

The simplest way to get both QEMU variants is to run:

```bash
just setup-qemu
```

This downloads the xPack QEMU release (currently 9.2.4-1) and extracts it to `tests/xpack-qemu-arm-*/`. The tarball contains both `qemu-system-arm` (mainline) and `qemu-system-gnuarmeclipse` (legacy). The Justfile automatically prefers these local binaries over anything on your system PATH.

To verify what binaries the Justfile will use:

```bash
just qemu-status
```

### Mainline QEMU

`qemu-system-arm` 9.2.4+ is the preferred emulator for all new platforms. It supports MPS2, MPS3, Olimex, and micro:bit machines natively. Mainline QEMU does **not** exhibit the Flash-to-RAM call quirk seen in the legacy fork.

All mainline test recipes pass `-semihosting` in `UDYNLINK_QEMU_EXTRA_FLAGS`. Without semihosting, the `_sys_write0` calls in `semihosting.c` will silently fail and no output will reach the console.

**Quick setup:** Run `just setup-qemu` to download and extract the xPack release into `tests/`. The Justfile will prefer this local copy.

### Legacy xPack QEMU

`qemu-system-gnuarmeclipse` was historically used for STM32F429 testing. It is significantly faster for that specific board because of STM32-specific optimizations in the fork. However, xPack discontinued the fork in release 9.2.4; newer xPack installs only ship `qemu-system-arm`.

**Quick setup:** Run `just setup-qemu-legacy` to download and extract the last xPack release (7.2.5-1) that includes `qemu-system-gnuarmeclipse` into `tests/`. The Justfile will prefer this local copy.

If you need the legacy binary manually, use an older xPack release (7.2.5-1) from the `xpack-dev-tools/qemu-arm-xpack` project.

### Known Quirks

**Flash-to-RAM call quirk (legacy QEMU only):** On `stm32f103_bluepill` and `stm32f051_discovery`, modules that call host firmware functions (e.g. `printf`) hang when the host firmware resides in flash. This is a `qemu-system-gnuarmeclipse` emulation bug. Mainline QEMU is not affected. Internal module calls (flash-to-flash or RAM-to-RAM) work fine on both.

## Platform Test Matrix

| Platform | QEMU Machine | QEMU Binary | CPU | Status | Notes |
|----------|--------------|-------------|-----|--------|-------|
| `stm32f429_discovery` | STM32F429I-Discovery | `qemu-system-gnuarmeclipse` | cortex-m4 | Passing | Fast baseline, legacy xPack fork (62 pass; `test-strip-init-array` skipped) |
| `mps2_an386` | mps2-an386 | `qemu-system-arm` (9.2.4+) | cortex-m4 | Passing | Mainline QEMU, ~0.5 s per test (64 pass) |
| `olimex_stm32_h405` | olimex-stm32-h405 | `qemu-system-arm` | cortex-m4f | Passing | Hard-float M4F on mainline QEMU (64 pass) |
| `mps2_an385` | mps2-an385 | `qemu-system-arm` (9.2.4+) | cortex-m3 | Passing | Mainline QEMU (64 pass) |
| `mps2_an500` | mps2-an500 | `qemu-system-arm` (9.2.4+) | cortex-m7 | Passing | Mainline QEMU (64 pass) |
| `mps2_an505` | mps2-an505 | `qemu-system-arm` (9.2.4+) | cortex-m33 | Passing | Mainline QEMU, secure boot (see below) (64 pass) |
| `microbit` | microbit | `qemu-system-arm` | cortex-m0 | Broken | QEMU microbit machine does not support ELF `-kernel` at 0x00000000 |
| `stm32f103_bluepill` | NUCLEO-F103RB | `qemu-system-gnuarmeclipse` | cortex-m3 | Partial | Boots, internal calls OK; Flash-to-RAM host calls hang (QEMU quirk) |
| `stm32f051_discovery` | STM32F0-Discovery | `qemu-system-gnuarmeclipse` | cortex-m0 | Partial | Boots, internal calls OK; same Flash-to-RAM quirk as M3 |

**MPS2-AN505 (Cortex-M33) note:** QEMU boots the Cortex-M33 in Secure state and fetches the initial vector table from the secure alias address `0x10000000`. The platform linker script (`tests/platforms/mps2_an505/mem.ld`) places the vector table at `0x10000000` so that `-kernel` loading works directly.

**microbit note:** QEMU's `microbit` machine does not properly load ELF files via `-kernel` at `0x00000000`. It needs a raw binary loaded via `-device loader,file=...,addr=0x0`. The test harness currently does not support this mode.

## How to Add a New Test Case

A test case is a self-contained directory under `tests/test-<name>/` with three parts: module source, a `test_data.py` descriptor, and a `test_qemu.c` harness.

### Step 1: Create the directory

```bash
mkdir tests/test-mycase
```

### Step 2: Write the module source

`tests/test-mycase/mod_foo.c`:

```c
#include <stdio.h>

int hello(int arg) {
    printf("Hello World! arg=%d\n", arg);
    return -arg;
}

int test(void) {
    return (hello(0) == 0 && hello(10) == -10);
}
```

### Step 3: Write `test_data.py`

`tests/test-mycase/test_data.py`:

```python
test_data = {
    "desc": "Simple 'hello world' test",
    "modules": [["mod_foo.c"]],
    "required": [r"^Hello World! arg=0$", r"^Hello World! arg=10$"]
}
```

- `desc` — human-readable description printed during the run.
- `modules` — list of module build commands. Each inner list is a set of source files passed to `mkmodule` in a single invocation.
- `required` — list of regexes that must appear in the QEMU output. By default each regex must match once per load mode (3 times total). You can override this with `total_loads`.

### Step 4: Write `test_qemu.c`

`tests/test-mycase/test_qemu.c`:

```c
#include "udynlink.h"
#include "mod_foo_module_data.h"
#include "test_utils.h"
#include <stdio.h>

int test_qemu(void) {
    const char *exported_syms[] = {"hello", "test", NULL};
    const char *extern_syms[] = {"printf", NULL};
    udynlink_module_t mod;
    int res = 0;

    for (int i = (int)UDYNLINK_LOAD_MODE_COPY_ALL;
         i <= (int)UDYNLINK_LOAD_MODE_XIP; i++) {
        if (udynlink_load_module(&mod, mod_foo_module_data, NULL, 0,
                                 (udynlink_load_mode_t)i))
            return 0;
        CHECK_RAM_SIZE(&mod, 0);
        if (!check_exported_symbols(&mod, exported_syms))
            goto exit;
        if (!check_extern_symbols(&mod, extern_syms))
            goto exit;
        if (!run_test_func(&mod))
            goto exit;
        udynlink_unload_module(&mod);
    }
    res = 1;
exit:
    udynlink_unload_module(&mod);
    return res;
}
```

The function `test_qemu()` is called by `tests/qemu_host/src/main.c`. It must return non-zero on success and zero on failure. `main.c` prints `*** TEST OK ***` or `*** TEST FAILED! ***` based on this return value.

### Test Utilities (`test_utils.h`)

The following helpers are available from `tests/qemu_host/src/test_utils.h`:

| Helper | Purpose |
|--------|---------|
| `check_exported_symbols(p_mod, slist)` | Verify every name in the NULL-terminated array is present and typed `UDYNLINK_SYM_TYPE_EXPORTED`. |
| `check_extern_symbols(p_mod, slist)` | Verify every name is present and typed `UDYNLINK_SYM_TYPE_EXTERN`. |
| `run_test_func(p_mod)` | Set `r9` to `p_mod->ram_base` via `UDYNLINK_PREPARE_CALL()`, look up the symbol `test`, and call it. |
| `CHECK_RAM_SIZE(p, s)` | Macro that fails the test (via `goto exit`) if `data_size + bss_size < s`. |
| `test_load_module(...)` | Wrapper around `udynlink_load_module` that also registers the module in the global test table. |
| `test_unload_module(...)` | Wrapper around `udynlink_unload_module` that unregisters the module first. |

For C++ modules, call `udynlink_cpp_init(p_mod)` after `test_load_module()` and before `run_test_func()`.

### Auto-Detection

If a test directory does not contain `test_data.py`, the driver falls back to auto-detection: it scans the directory for `.cpp` files and creates a default `test_data` with an empty description, no required regexes, and the discovered source as a single module. In practice, most tests should provide an explicit `test_data.py` for reliable validation.

### Multi-Module Tests

Tests that load more than one module must use `test_load_module()` and `test_unload_module()` instead of calling the raw loader API. These wrappers maintain a global module table (`g_modules[]` in `main.c`) and can be used together with `test_resolve_symbol()` (a weak symbol in the test host) to implement custom cross-module symbol resolution.

`UDYNLINK_MAX_MODULES` defaults to 8 in the test host firmware. You can override it at CMake time if your test loads more modules.

### Cross-Module Test

The dependency system is validated by a family of tests, all exercising all three load modes (COPY_ALL, COPY_TEXT_DATA, XIP) at both `-O3` and `-Os`:

- `test-cross-module` — loads `mod_math` and `mod_app`, verifies `mod_app` can call functions exported by `mod_math` via gateway/stub thunks. Uses `udynlink_dep_load()`, `udynlink_dep_unload()`, and a custom `test_resolve_symbol()` that delegates to `udynlink_dep_resolve_func()` and `udynlink_dep_resolve_data()`. `mod_math` declares a preallocated thunk export for `math_add` (`UDYNLINK_THUNK_GATEWAY()`/`UDYNLINK_THUNK_EXPORT`), so the test asserts both new behaviors: the in-module thunk exists right after load (before any importer) and `math_add` is served from it while undeclared `math_mul` still falls back to the dynamic pool (`pool used = 10`).
- `test-dep-auto-load` — verifies automatic dependency loading via `udynlink_external_dep_load()` when a required module is not yet registered.
- `test-dep-stub-dedup` — two modules importing the same function share a single stub; asserts the exact pool accounting (`pool used = 10`).
- `test-dep-circular` — circular dependencies (`mod_a` requires `mod_b` and vice versa) are deferred via `UDYNLINK_SYM_DEFERRED` and both modules still load.
- `test-dep-data` — cross-module data variables resolve via `udynlink_dep_resolve_data()` without thunks.
- `test-dep-missing` — an unsatisfied dependency fails the load with `UDYNLINK_ERR_LOAD_UNKNOWN_SYMBOL`.

## How to Add a New QEMU Platform

Adding a new platform means creating a new directory under `tests/platforms/<name>/` with four files, then wiring it into the `Justfile`.

### Required Files

| File | Purpose |
|------|---------|
| `platform.cmake` | Sets `UDYNLINK_PLATFORM_*` variables consumed by `tests/qemu_host/CMakeLists.txt`. |
| `mem.ld` | Linker script defining memory regions and section placement. |
| `startup.s` | Assembly vector table and `Reset_Handler` (zero BSS, call `main`). |
| `semihosting.c` | Newlib syscalls and ARM semihosting output implementation. |

### Reference: MPS2-AN386

`tests/platforms/mps2_an386/platform.cmake`:

```cmake
set(UDYNLINK_PLATFORM_CFLAGS
    -mcpu=cortex-m4
    -mthumb
    -mfloat-abi=soft
)

set(UDYNLINK_PLATFORM_COMPILE_OPTIONS
    -Og
    -fmessage-length=0
    -fsigned-char
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -g3
)

set(UDYNLINK_PLATFORM_DEFINES
    UDYNLINK_HOST_ARCH_TAG=UDYNLINK_ARCH_TAG_CORTEX_M4
)

set(UDYNLINK_PLATFORM_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
)

set(UDYNLINK_PLATFORM_LINK_OPTIONS
    -Wl,-T,mem.ld
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map,test1.map
    --specs=nano.specs
)

set(UDYNLINK_PLATFORM_LINK_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
)

set(UDYNLINK_PLATFORM_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/startup.s
    ${CMAKE_CURRENT_LIST_DIR}/semihosting.c
)

set(UDYNLINK_PLATFORM_HAS_CXX FALSE)
```

**CMake variables explained:**

- `UDYNLINK_PLATFORM_CFLAGS` — target CPU and ABI flags passed to both C and C++ compilation.
- `UDYNLINK_PLATFORM_COMPILE_OPTIONS` — generic compile options (optimization, warnings, debug).
- `UDYNLINK_PLATFORM_DEFINES` — preprocessor definitions. `UDYNLINK_HOST_ARCH_TAG` must match the MCU core family and float ABI.
- `UDYNLINK_PLATFORM_INCLUDE_DIRS` — extra `-I` paths for the platform directory itself.
- `UDYNLINK_PLATFORM_LINK_OPTIONS` — linker flags, including the linker script path.
- `UDYNLINK_PLATFORM_LINK_DIRS` — directories searched for `-T` scripts.
- `UDYNLINK_PLATFORM_SOURCES` — additional source files (startup, semihosting) linked into every test ELF.
- `UDYNLINK_PLATFORM_HAS_CXX` — set to `TRUE` if the platform provides a C++ runtime (e.g. `__cxa_atexit` stubs).

`tests/platforms/mps2_an386/mem.ld` places everything in a single 8 MB RAM region at `0x00000000` so that `-kernel` loads the ELF segments at the addresses specified in the program headers:

```ld
MEMORY
{
  RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 8M
}
```

`tests/platforms/mps2_an386/startup.s` provides the standard Cortex-M vector table and a `Reset_Handler` that zeros `.bss` before calling `main`. `tests/platforms/mps2_an386/semihosting.c` implements buffered output via `SYS_WRITE0` (`bkpt 0xAB`) and the Newlib syscall stubs (`_write`, `_sbrk`, `_exit`, etc.).

### Special Cases

**MPS2-AN505 (Cortex-M33):** QEMU boots the M33 in Secure state and fetches the vector table from `0x10000000` (the secure alias). The linker script must place `.isr_vector` at that address rather than `0x00000000` so that `-kernel` loading works.

**microbit (Cortex-M0):** QEMU's `microbit` machine does not load ELF files correctly with `-kernel` at `0x00000000`. It requires a raw binary loaded via `-device loader,file=firmware.bin,addr=0x0`. The current test harness does not support raw-binary output, so this platform is listed as broken.

### Justfile Recipe

Add a recipe so users can run the new platform without memorizing environment variables:

```just
# Run all tests on MyPlatform (Cortex-M4) - mainline QEMU
test-myplatform:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=my_platform \
    UDYNLINK_QEMU_BIN=qemu-system-arm \
    UDYNLINK_QEMU_MACHINE=my-machine \
    UDYNLINK_QEMU_CPU=cortex-m4 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py
```

If the platform needs a different module target (e.g. Cortex-M7), add `UDYNLINK_MODULE_TARGET=cortex-m7` as well.

## Debugging Failing Tests

### Manual QEMU Launch

Build the test first, then run QEMU directly to inspect output or attach a debugger:

```bash
just build-tests mps2_an386
just qemu-mps2          # run without GDB
just qemu-mps2-gdb      # halt and start GDB server on port 1234
```

For the legacy STM32F429 platform:

```bash
just build-tests stm32f429_discovery
just qemu-f429
```

### Enabling Debug Logging

Set the `UDYNLINK_TEST_DEBUG` environment variable when running through `just`:

```bash
UDYNLINK_TEST_DEBUG=1 just test-mps2-single test-globals1
```

This adds `-DUDYNLINK_TEST_DEBUG_LEVEL=UDYNLINK_DEBUG_INFO` to the CMake build, causing the loader to print verbose relocation and symbol-resolution traces to semihosted stdout.

### Reading Driver Logs

For every test run, `test_driver.py` writes three log files into the test directory:

- `output_build_<opt>.txt` — `mkmodule` compiler and linker output (`O3` or `Os`).
- `output_objdump_<opt>.txt` — Disassembly of the compiled module ELF.
- `output_test_<opt>.txt` — Full QEMU stdout/stderr.

If a test fails, read `output_test_Os.txt` (or `output_test_O3.txt`) to see the exact crash message or missing regex.

### Common Failure Modes

| Symptom | Likely Cause |
|---------|--------------|
| "Can't find the test OK indicator in the output" | The test crashed, hung, or timed out before printing `*** TEST OK ***`. Check `output_test_*.txt` for HardFault or assertion messages. |
| "Can't find '<regex>' in output" | The `required` regex in `test_data.py` does not match the actual QEMU output. Verify the regex and the module's `printf` output. |
| "Unable to compile module(s)" | Syntax error in module source, missing toolchain, or unsupported `mkmodule` flags. Check `output_build_*.txt`. |
| "Unable to run QEMU or timeout running" | QEMU binary not found, wrong `-machine` name, or semihosting not enabled (mainline QEMU requires `-semihosting`). |

## Test Driver Internals

`tests/test_driver.py` orchestrates the full test lifecycle. The flow for a single `(test_name, opt)` pair is:

1. **Scan** all directories matching `test-*`.
2. **Read** `test_data.py` (or auto-detect `.cpp` sources).
3. **Create isolated working directories** so parallel test suites do not collide:
   - `tests/build_<platform>_<test>_<opt>/` — CMake build directory.
   - `tests/build_<platform>_<test>_<opt>_src/` — copied source files and module build artifacts.
4. **Compile modules** by invoking `../../scripts/mkmodule` inside the isolated src directory, passing `-I<repo>/udynlink` so module sources can `#include` udynlink headers (e.g. `udynlink_deps_api.h`). Generates `.bin`, `.elf`, and `*_module_data.h`.
5. **Run objdump** on the module ELF and save disassembly to `output_objdump_*.txt`.
6. **Configure and build** the QEMU host firmware via CMake, pointing `UDYNLINK_TEST_SRC_DIR` at the isolated src directory.
7. **Launch QEMU** with the freshly built `test1.elf`, using the platform-specific QEMU command line built by `build_qemu_cmd()`.
8. **Validate** the captured output for `*** TEST OK ***` and regex matches.

Build artifacts are kept between runs of the same test to allow incremental CMake rebuilds. The `src` directory is always recreated because it holds per-test generated headers. To force a full clean, set `UDYNLINK_TEST_CLEAN=1` or pass `--clean`.

For a deeper look at the core loader logic exercised by these tests, see [How It Works](how-it-works.md) and [API Reference](api-reference.md). For real-world module authoring patterns, see [Module Guide](writing-modules.md) and [Examples](examples.md).

## Adding a New Cortex-M Target

The udynlink toolchain supports nine Cortex-M targets out of the box. Adding a new core (for example a future Cortex-M variant) requires updating the target database in `scripts/targets.py` and selecting the correct assembly template.

### The Target Database

Location: `scripts/targets.py`

The `TARGETS` dictionary maps a target name to a metadata dictionary:

```python
TARGETS = {
    "cortex-m4": {
        "mcpu": "cortex-m4",
        "arch": "armv7e-m",
        "fpu": None,
        "float_abi": "soft",
        "arch_tag": _make_arch_tag(4, False, "soft"),
        "template": "asm_template_armv7m.tmpl",
    },
    ...
}
```

Each entry contains:

| Key | Meaning |
|-----|---------|
| `mcpu` | GCC `-mcpu` flag value |
| `arch` | ARM architecture profile (informational) |
| `fpu` | FPU type for `-mfpu=`, or `None` / `"auto"` |
| `float_abi` | Float ABI: `soft`, `softfp`, or `hard` |
| `arch_tag` | 16-bit architecture tag written into the module header |
| `template` | Jinja2 assembly template for exported-function prologues |

The architecture tag is computed by `_make_arch_tag(family, fpu, float_abi)`:

```python
def _make_arch_tag(family, fpu, float_abi):
    tag = family & 0x0F
    if fpu:
        tag |= 0x10
    abi_code = {"soft": 0, "softfp": 1, "hard": 2}
    tag |= (abi_code.get(float_abi, 0) & 0x03) << 5
    return tag
```

Bit layout of the 16-bit tag:

| Bits | Field |
|------|-------|
| [3:0] | Core family ID |
| 4 | FPU present (1 = yes) |
| [6:5] | Float ABI (00 = soft, 01 = softfp, 10 = hard) |
| [15:7] | Reserved |

Family IDs currently in use:

| ID | Core |
|----|------|
| 1 | Cortex-M0 |
| 2 | Cortex-M0+ |
| 3 | Cortex-M3 |
| 4 | Cortex-M4 / Cortex-M4F |
| 7 | Cortex-M7 |
| 8 | Cortex-M33 |
| 9 | Cortex-M55 |
| 10 | Cortex-M85 |

### Step-by-Step: Adding a New Target

1. **Choose a target name.** Use lowercase with hyphens, matching GCC convention (e.g. `cortex-m55`).
2. **Determine the GCC `-mcpu` flag.** For `cortex-m55` this is `cortex-m55`.
3. **Determine the ARM architecture profile.** For Cortex-M55 this is `armv8.1-m.main`.
4. **Determine FPU and float ABI settings.** Cortex-M55 uses `fpu="auto"` and `float_abi="hard"`.
5. **Calculate the architecture tag.** Using `_make_arch_tag(9, True, "hard")` gives `0x59`.
6. **Select the right assembly template.**
   - `asm_template_armv6m.tmpl` for ARMv6-M (M0 / M0+)
   - `asm_template_armv7m.tmpl` for ARMv7-M / ARMv7E-M (M3 / M4 / M7)
   - `asm_template_armv8m.tmpl` for ARMv8-M (M33 / M55 / M85)

   Cortex-M55 is ARMv8-M, so use `asm_template_armv8m.tmpl`.
7. **Add the entry to the `TARGETS` dict:**

```python
"cortex-m55": {
    "mcpu": "cortex-m55",
    "arch": "armv8.1-m.main",
    "fpu": "auto",
    "float_abi": "hard",
    "arch_tag": _make_arch_tag(9, True, "hard"),
    "template": "asm_template_armv8m.tmpl",
},
```

8. **Verify with `python3 targets.py`.** Running the script as a module prints a Markdown table of all targets:

```bash
cd scripts
python3 targets.py
```

9. **Validate by compiling a test module:**

```bash
python3 mkmodule --target cortex-m55 hello.c
```

If the compilation succeeds and produces `hello.bin`, the target is correctly registered.

### Assembly Templates

Exported functions in a loadable module receive an assembly prologue that loads `r9` from the fixed LOT base address before jumping to the real function body. The prologue is generated from a Jinja2 template chosen per target.

Templates live in `scripts/asm_template_*.tmpl`. There are three families:

| Template | Architecture | Cores | Characteristics |
|----------|--------------|-------|-----------------|
| `asm_template_armv6m.tmpl` | ARMv6-M | M0, M0+ | Thumb-1 only; no `ldr.w` or other wide instructions |
| `asm_template_armv7m.tmpl` | ARMv7-M / ARMv7E-M | M3, M4, M4F, M7 | Thumb-2; uses `ldr.w` for 32-bit literal loads |
| `asm_template_armv8m.tmpl` | ARMv8-M | M33, M55, M85 | Thumb-2; similar to v7m template |

If a future core introduces a new instruction set extension that requires different prologue code, create a new template file (e.g. `asm_template_armv9m.tmpl`) and reference it in the target entry.

### QEMU Test Host Considerations

A target entry in `scripts/targets.py` does **not** require a corresponding QEMU test host. The target database is primarily a compilation-time mapping. For example, `cortex-m55` and `cortex-m85` are present in the database so that modules can be built for them, but upstream QEMU does not yet provide board models for these cores.

You can validate that a new target compiles correctly without running it under QEMU:

```bash
just validate-all-targets
```

This compiles `hello.c` for every entry in `TARGETS` and reports any failures. To add full integration testing for a new core, you would also need to create a QEMU platform directory under `tests/platforms/` (see [How to Add a New QEMU Platform](#how-to-add-a-new-qemu-platform) above).

### Architecture Tag Compatibility

The `arch_tag` field in the module header is validated at load time against `UDYNLINK_HOST_ARCH_TAG`, a compile-time constant defined in the host firmware. The loader rejects a module that is incompatible: the core family must match, and the module's float ABI must not be stricter than the host's (e.g., a hard-float module will not load on a soft-float host).

This means:

- A module built for `cortex-m4f` (arch_tag `0x54`) will **not** load on a `cortex-m4` host (arch_tag `0x04`), because the FPU and float ABI bits differ.
- A module built for `cortex-m0` (arch_tag `0x01`) will **not** load on a `cortex-m3` host (arch_tag `0x03`), because the family ID differs.

When defining `UDYNLINK_HOST_ARCH_TAG` in your host firmware, it must exactly match the core + FPU + float ABI of the target MCU. See [How It Works](how-it-works.md) for architecture tag details and [Module Guide](writing-modules.md) for target selection.