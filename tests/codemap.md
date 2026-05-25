# tests/

## Responsibility
Integration test suite for udynlink, validating the full pipeline from C/C++ source → loadable module → QEMU execution on a simulated STM32F429I-Discovery board. Tests cover all three load modes (`COPY_ALL`, `COPY_CODE`, `XIP`) and both optimization levels (`-O0`, `-Os`).

## Design Patterns
- **Data-Driven Test Cases**: Each `test-*/` directory contains a `test_data.py` dict describing module sources, expected output regexes, and load count.
- **Parameterized Execution**: `test_driver.py` runs every test twice (with and without optimization) and across all three load modes inside each `test_qemu.c` harness.
- **Host Firmware Simulation**: `qemu_host/` is a full Eclipse-generated ARM Cortex-M4 project that provides `udynlink_externals.h` implementations (malloc/free, printf, symbol resolution) and runs under QEMU.
- **Golden Output Matching**: Tests pass only if QEMU output contains `*** TEST OK ***` and all regexes from `test_data["required"]` match the expected number of times (once per load mode).

## Data & Control Flow
1. **Test Driver** (`test_driver.py`):
   - Scans directories matching `test-*`.
   - For each test, reads `test_data.py` (or auto-detects `.cpp`/`.c` sources).
   - Invokes `../../scripts/mkmodule` to compile module sources into `.bin` + `*_module_data.h`.
   - Copies `test_qemu.c` from the test directory into `qemu_host/src/`.
   - Builds `test1.elf` in `qemu_host/Debug/` via Eclipse-generated makefiles.
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
- **Depends on**: `scripts/mkmodule` (toolchain), `qemu-system-gnuarmeclipse` (QEMU), `arm-none-eabi-gcc`, Python 3 with `subprocess32`, `pyelftools`, `Jinja2`.
- **Build System**: Eclipse-generated makefiles in `qemu_host/Debug/` (do not hand-edit `subdir.mk` or `sources.mk`).
- **udynlink Source**: The Debug build pulls `udynlink.c` from `../../../udynlink/` via relative path in `udynlink/subdir.mk`.

## Directory Structure
| Path | Purpose |
|------|---------|
| `test_driver.py` | Main test orchestrator: compile modules, build QEMU host, run, validate. |
| `test.sh` | Shell wrapper that sets PATH for QEMU and cleans up build artifacts after running. |
| `qemu_host/` | Full STM32F4 Eclipse project with HAL, Newlib, and udynlink integration. |
| `qemu_host/src/main.c` | Host firmware entry point; implements `udynlink_externals.h`. |
| `qemu_host/src/test_utils.c/h` | Helper functions for symbol checking and test function invocation. |
| `qemu_host/Debug/makefile` | Eclipse-generated top-level makefile linking the test ELF. |
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
- **Optimization modes**: Every test runs once with `--no-opt` (`-O0`) and once with default (`-Os`).
- **Build artifacts**: Generated `.o`, `.elf`, `.bin`, `*_module_data.h`, and `output_*.txt` logs are cleaned by `test.sh`.
- **CI**: `.github/workflows/ci.yml` runs `python ./test_driver.py` in the `tests/` directory on every push.
