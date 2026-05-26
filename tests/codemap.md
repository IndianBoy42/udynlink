# tests/

## Responsibility
Integration test suite for udynlink, validating the full pipeline from C/C++ source → loadable module → QEMU execution on a simulated STM32F429I-Discovery board. Tests cover all three load modes (`COPY_ALL`, `COPY_CODE`, `XIP`) and both optimization levels (`-O0`, `-Os`).

## Design Patterns
- **Data-Driven Test Cases**: Each `test-*/` directory contains a `test_data.py` dict describing module sources, expected output regexes, and load count.
- **Parameterized Execution**: `test_driver.py` runs every test twice (with and without optimization) and across all three load modes inside each `test_qemu.c` harness.
- **Host Firmware Simulation**: `qemu_host/` is a full ARM Cortex-M4 CMake project (via `tests/qemu_host/CMakeLists.txt`) that provides `udynlink_externals.h` implementations (malloc/free, printf, symbol resolution) and runs under QEMU.
- **Platform Abstraction**: Target-specific compile/link flags live in `cmake/platforms/<name>.cmake` files (e.g., `stm32f429_discovery.cmake` for the STM32F429I-Discovery board), making it straightforward to add support for new MCU families.
- **Standalone Build**: `tests/qemu_host/CMakeLists.txt` can build standalone (auto-downloads the core udynlink library) or as part of an in-tree build from the repo root.
- **Golden Output Matching**: Tests pass only if QEMU output contains `*** TEST OK ***` and all regexes from `test_data["required"]` match the expected number of times (once per load mode).

## Data & Control Flow
1. **Test Driver** (`test_driver.py`):
   - Scans directories matching `test-*`.
   - For each test, reads `test_data.py` (or auto-detects `.cpp`/`.c` sources).
   - Invokes `../../scripts/mkmodule` to compile module sources into `.bin` + `*_module_data.h`.
   - Copies `test_qemu.c` from the test directory into `qemu_host/src/`.
   - Builds `test1.elf` via CMake in `tests/build/`:
     ```
     cmake -B build -S ../qemu_host -DUDYNLINK_BUILD_TESTS=ON
     cmake --build build --target test1.elf
     ```
   - Runs `qemu-system-gnuarmeclipse -board STM32F429I-Discovery -image test1.elf -nographic`.
   - Validates output for `*** TEST OK ***` and required regex matches.
2. **QEMU Host Firmware** (`qemu_host/src/main.c`):
   - Implements all `udynlink_externals.h` hooks:
     - `udynlink_external_malloc` → `malloc`
     - `udynlink_external_free` → `free`
     - `udynlink_external_vprintf` → `vprintf`
     - `udynlink_external_resolve_symbol` → resolves `printf` statically; delegates to weak `test_resolve_symbol` for test-specific symbols.
   - Calls `test_qemu()` (injected per-test) and prints `*** TEST OK ***` or `*** TEST FAILED! ***`.
3. **Per-Test Harness** (`test_qemu.c` in each `test-*/`):
   - Iterates over all load modes (`_UDYNLINK_LOAD_MODE_FIRST` to `_UDYNLINK_LOAD_MODE_LAST`).
   - Loads module, checks RAM size, validates exported/extern symbols, runs test functions.
   - For C++ tests: calls `udynlink_cpp_init(&mod)` before running functions.
   - Unloads module after each mode.

## Integration Points
- **Depends on**: `scripts/mkmodule` (toolchain), `qemu-system-gnuarmeclipse` (QEMU), `arm-none-eabi-gcc`, CMake ≥ 3.16, Python 3 with `subprocess32`, `pyelftools`, `Jinja2`.
- **Build System**: CMake via `tests/qemu_host/CMakeLists.txt`, which includes platform-specific flags from `cmake/platforms/<name>.cmake` (selected by `-DUDYNLINK_PLATFORM=<name>`).
- **Core Library**: The test host `CMakeLists.txt` uses a `if(NOT TARGET udynlink)` guard to either pull `udynlink.c` from the repo root (standalone build) or link against the existing target (in-tree build).

## Directory Structure
| Path | Purpose |
|------|---------|
| `test_driver.py` | Main test orchestrator: compile modules, build QEMU host, run, validate. |
| `test.sh` | Shell wrapper that sets PATH for QEMU and cleans up build artifacts after running. |
| `qemu_host/` | Full STM32F4 CMake project with HAL, Newlib, and udynlink integration. |
| `qemu_host/src/main.c` | Host firmware entry point; implements `udynlink_externals.h`. |
| `qemu_host/src/test_utils.c/h` | Helper functions for symbol checking and test function invocation. |
| `qemu_host/CMakeLists.txt` | Standalone-capable CMake project that builds the QEMU test ELF. Include tree via `add_subdirectory` or build standalone. |
| `build/` | CMake out-of-tree build directory (created by `cmake -B tests/build`). |
| `test-helloworld/` | Basic C module test: exported `hello`/`test` functions, extern `printf`. |
| `test-helloworld-cpp/` | C++ module test: validates global constructors, `-fno-exceptions`, `test2` with args. |
| `test-fib/` | Function recursion and local computation test. |
| `test-globals1/` / `test-globals2/` | Global variable read/write across load modes. |
| `test-global-ptrs1/` / `test-global-ptrs2/` | Global pointer relocation tests. |
| `test-global-string-arrays/` | String and array data relocation tests. |
| `test-local-ptrs/` | Local pointer handling test. |
| `test-multiple-relocs/` | Multiple relocations to the same symbol (regression test for eh2k [10]). |
| `test-three-files/` | Multi-source module compilation test. |

## Key Configuration
- **Default timeout**: 5 seconds per QEMU run (`default_qemu_timeout` in `test_driver.py`).
- **Optimization modes**: Every test runs once with `-O 0` and once with default (`-Os`).
- **Build system**: CMake. The platform is selected via `-DUDYNLINK_PLATFORM=<name>` (default: `stm32f429_discovery`). Platform files live in `cmake/platforms/`.
- **Build artifacts**: Generated `.o`, `.elf`, `.bin`, `*_module_data.h`, `output_*.txt` logs, and CMake build directory `build/` are cleaned by `test.sh`.
- **CI**: `.github/workflows/ci.yml` runs `python ./test_driver.py` in the `tests/` directory on every push.
