# Plan: Migrate from Eclipse Makefiles to CMake and Mainline Toolchains

## Goal
Remove all Eclipse IDE dependencies/assumptions from the `udynlink` repository and replace the build system with CMake. Ensure:
- The core `udynlink` library is a first-class CMake target usable by downstream CMake projects.
- The QEMU test host firmware is built via CMake in a unified build directory.
- CI and local development workflows use standard, mainline compiler toolchains and do not rely on Eclipse-generated artifacts.
- The architecture is designed for **easy addition of new target architectures** (Cortex-M0+/M3/M4/M7/M33) and **platforms** (other STM32 families, NXP, Nordic nRF, etc.).

---

## Decisions (User Reviewed)

| Topic | Decision |
|-------|----------|
| **QEMU** | Keep xPack QEMU fork (`qemu-system-gnuarmeclipse`) for now. Remove Eclipse IDE assumptions, but do not attempt porting to a mainline-QEMU-supported board. |
| **CMake scope** | Add CMake for **both** the core `udynlink` library *and* the QEMU test host firmware. The library must be easily consumable from external CMake projects with idiomatic configuration. Tests can be built **standalone** or **in-tree**. |
| **Compiler prefix** | Environment variable `UDYNLINK_CC_PREFIX` (default `arm-none-eabi-`) in Python `mkmodule`. CMake uses standard `CMAKE_C_COMPILER` variables (or defaults to `arm-none-eabi-gcc`). |
| **Build directory** | Unified `tests/build/` directory for the QEMU host firmware. Root library can be built standalone. |
| **CI / QEMU provisioning** | Switch to a **pre-installed runner image** that already contains xPack QEMU. Remove the `wget`/`tar` QEMU download step from `.github/workflows/ci.yml`. |
| **Multi-target** | `mkmodule` gains a `--mcpu` flag. CMake gains a `UDYNLINK_PLATFORM` option. The core library remains target-agnostic. |

---

## Architecture

### Repository Layout After Migration

```
udynlink/
├── CMakeLists.txt                  # Root CMake: library target + optional tests
├── cmake/
│   ├── udynlinkConfig.cmake.in     # CMake package config template
│   └── platforms/                  # Platform-specific CMake modules (extensible)
│       └── stm32f429_discovery.cmake # Current QEMU test host platform file
├── udynlink/
│   ├── udynlink.c                  # Core library (target-agnostic)
│   ├── udynlink.h
│   └── udynlink_externals.h
├── scripts/
│   ├── mkmodule                     # Updated: --mcpu, UDYNLINK_CC_PREFIX
│   ├── udynlink_utils.py
│   ├── asm_template.tmpl            # Will need --mcpu flag too
│   └── code_before_data.ld          # Linker script (platform-agnostic layout)
├── tests/
│   ├── test_driver.py               # Updated: CMake + QEMU lookup
│   ├── test.sh                      # Updated paths
│   └── qemu_host/
│       ├── CMakeLists.txt           # Standalone-capable firmware project
│       ├── src/                     # Test app sources (main.c, test_utils.c, ...)
│       ├── system/                  # HAL/CMSIS/Newlib/diag for current board
│       │   ├── include/
│       │   └── src/
│       └── ldscripts/               # Linker scripts for current board
└── .github/
    └── workflows/
        └── ci.yml                   # Updated: cmake, pre-installed QEMU
```

**Design principle**: The core library has **zero** target-specific code. Platform-specific logic lives in:
1. `cmake/platforms/*.cmake` — CMake toolchain/platform presets.
2. `tests/qemu_host/system/` — HAL, CMSIS, startup, linker scripts for the current test board.
3. `scripts/mkmodule` — build flags for the module compiler.

---

### Core Library CMake Target (`udynlink`)

- **Target type**: `add_library(udynlink STATIC udynlink/udynlink.c)`
- **Public headers**: `udynlink/udynlink.h`, `udynlink/udynlink_externals.h`
- **Include directories**: `udynlink/` (public interface)
- **Compile options**: **None target-specific.** The library is pure C and should compile for any ARM (or non-ARM) target. CPU/arch flags belong in the **consumer's toolchain file** or the **test host** target.
- **Install support**: `install(TARGETS udynlink ...)` + `install(EXPORT udynlinkTargets ...)` + `configure_package_config_file(...)` so that `find_package(udynlink)` works in downstream projects.
- **Tests option**: Root `CMakeLists.txt` contains:
  ```cmake
  option(UDYNLINK_BUILD_TESTS "Build QEMU test host firmware" OFF)
  if(UDYNLINK_BUILD_TESTS)
      add_subdirectory(tests/qemu_host)
  endif()
  ```

---

### Test Host Firmware CMake Target (`test1.elf`)

**Hybrid design**: `tests/qemu_host/CMakeLists.txt` can be built **standalone** or included from root.

```cmake
# Standalone safety: grab the library if not already a target
if(NOT TARGET udynlink)
    add_subdirectory(
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_BINARY_DIR}/udynlink
    )
endif()
```

#### Target definition
- **Target type**: `add_executable(test1.elf ...)`
- **Sources**: All files currently listed in the Eclipse `subdir.mk` files under `tests/qemu_host/src/`, `tests/qemu_host/system/src/*`, and `udynlink/udynlink.c`.
- **Platform abstraction**: Extract the current STM32F429-specific settings into `cmake/platforms/stm32f429_discovery.cmake`. Include it from `tests/qemu_host/CMakeLists.txt`:
  ```cmake
  set(UDYNLINK_PLATFORM "stm32f429_discovery" CACHE STRING "Target platform for test host")
  include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/platforms/${UDYNLINK_PLATFORM}.cmake)
  ```
  This makes adding a new board as simple as adding a new `.cmake` file in `cmake/platforms/`.

#### Platform file (`cmake/platforms/stm32f429_discovery.cmake`)
Responsible for:
- Setting `CMAKE_C_COMPILER` to `arm-none-eabi-gcc` (if not set).
- Setting `CMAKE_SYSTEM_NAME` / `CMAKE_SYSTEM_PROCESSOR` (optional, for toolchain file behavior).
- Setting target CPU flags: `-mcpu=cortex-m4`, `-mthumb`, `-mfloat-abi=soft`.
- Setting compile definitions: `DEBUG`, `USE_FULL_ASSERT`, `OS_USE_SEMIHOSTING`, `TRACE`, `OS_USE_TRACE_SEMIHOSTING_DEBUG`, `STM32F429xx`, `USE_HAL_DRIVER`, `HSE_VALUE=8000000`.
- Adding include paths for the STM32F4 HAL/CMSIS.
- Adding linker script search path and linker flags: `-T mem.ld`, `-T libs.ld`, `-T sections.ld`, `-nostartfiles`, `-Xlinker --gc-sections`, `--specs=nano.specs`.
- Handling the `_startup.c` extra define (`OS_INCLUDE_STARTUP_INIT_MULTIPLE_RAM_SECTIONS`) via `set_source_files_properties`.

This separation means a future `cmake/platforms/nrf52840_dk.cmake` or `cmake/platforms/lpc1768.cmake` can be added without touching the core library or the test host `CMakeLists.txt`.

#### Build directory
- `tests/build/` (out-of-tree, unified).

---

### `mkmodule` Toolchain Changes

1. **Compiler prefix**: Replace all hardcoded `arm-none-eabi-gcc`, `arm-none-eabi-g++`, `arm-none-eabi-objcopy`, `arm-none-eabi-objdump` with prefix from `UDYNLINK_CC_PREFIX` env var (default `arm-none-eabi-`).
2. **Target CPU**: Replace the hardcoded `-mcpu=cortex-m4` with a `--mcpu` CLI flag (default `cortex-m4`). Update `asm_cmd` and `compile_cmd` and `link_cmd` to use the value.
3. **Link script**: Keep using `code_before_data.ld` (it is layout-agnostic), but ensure the `--mcpu` flag is passed consistently.

---

### `test_driver.py` Changes

- Replace `cd ../qemu_host/Debug && make clean && make test1.elf` with:
  ```python
  cmake_build_dir = "../build"
  run_cmd(f"cmake -B {cmake_build_dir} -S ../qemu_host -DUDYNLINK_BUILD_TESTS=ON")
  run_cmd(f"cmake --build {cmake_build_dir} --target test1.elf")
  ```
- Update QEMU command resolution:
  - Look for `qemu-system-gnuarmeclipse` in `PATH`.
  - If not found, check fallback paths (e.g., `./xpack-qemu-arm-*/bin/qemu-system-gnuarmeclipse` or an env var `UDYNLINK_QEMU_PATH`).
  - Keep existing env vars `UDYNLINK_QEMU_BIN`, `UDYNLINK_QEMU_MACHINE`, `UDYNLINK_QEMU_CPU`, `UDYNLINK_QEMU_EXTRA_FLAGS` if they exist in the current harness (check before editing).

---

### CI Changes

- Remove the `wget`/`tar` step that downloads xPack QEMU.
- Add `cmake` to apt-get install list.
- Set `PATH` or `UDYNLINK_QEMU_BIN` if the runner image places QEMU in a non-standard location.
- Keep `gcc-arm-none-eabi` and Python deps.

---

## Task Breakdown (Atomic, Independent)

### Task 1: Make compiler prefix and target CPU configurable in `mkmodule`
- **Agent**: `agent` (moderate complexity, single-file)
- **Files**: `scripts/udynlink_utils.py`, `scripts/mkmodule`
- **Details**:
  - In `udynlink_utils.py`, read `os.environ.get("UDYNLINK_CC_PREFIX", "arm-none-eabi-")`.
  - Provide a helper `get_tool(name)` that returns `f"{prefix}{name}"`.
  - Update every `execute(...)` call in `mkmodule` and `udynlink_utils.py` that invokes `gcc`, `g++`, `objcopy`, or `objdump` to use the prefixed tool.
  - Add `--mcpu` argument to `mkmodule` (default `cortex-m4`). Replace all literal `cortex-m4` in `compile_cmd`, `asm_cmd`, `link_cmd` with the argument value.
- **Deliverable**: `mkmodule` works with a custom prefix and custom CPU (`--mcpu cortex-m0plus`).
- **Review**: Run `mkmodule` on `test-helloworld` sources with a dummy prefix and custom `--mcpu`, verify command lines.

### Task 2: Add root CMake for core `udynlink` library
- **Agent**: `agent` (CMake authoring, package config)
- **Files**: New `CMakeLists.txt` (repo root), new `cmake/udynlinkConfig.cmake.in`
- **Details**:
  - `project(udynlink C)`
  - `add_library(udynlink STATIC udynlink/udynlink.c)`
  - `target_include_directories(udynlink PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/udynlink> $<INSTALL_INTERFACE:include>)`
  - `install(TARGETS udynlink EXPORT udynlinkTargets ...)`
  - `install(FILES udynlink/udynlink.h udynlink/udynlink_externals.h DESTINATION include)`
  - `configure_package_config_file(...)` and `write_basic_package_version_file(...)` in `cmake/`.
  - Include `GNUInstallDirs`.
  - Add `option(UDYNLINK_BUILD_TESTS "Build QEMU test host firmware" OFF)` and conditional `add_subdirectory(tests/qemu_host)`.
- **Deliverable**: A downstream project can `add_subdirectory(path/to/udynlink)` or `find_package(udynlink)` and `target_link_libraries(myfw udynlink)`.
- **Review**: Build the library target in a temporary build dir and inspect installed headers + exported targets.

### Task 3: Create platform abstraction and test host CMake
- **Agent**: `agent` (multi-file, needs exactness)
- **Files**: New `cmake/platforms/stm32f429_discovery.cmake`, new `tests/qemu_host/CMakeLists.txt`, delete `tests/qemu_host/Debug/` recursively.
- **Details**:
  - **Transcribe** every source file from the Eclipse `subdir.mk` files into a list variable in `tests/qemu_host/CMakeLists.txt`.
  - **Transcribe** include paths, defines, compile flags, and link flags into `cmake/platforms/stm32f429_discovery.cmake`.
  - The test host `CMakeLists.txt` should:
    - Set `UDYNLINK_PLATFORM` cache variable (default `stm32f429_discovery`).
    - Include the platform file from `cmake/platforms/${UDYNLINK_PLATFORM}.cmake`.
    - Include the standalone `if(NOT TARGET udynlink)` guard.
    - Call `add_executable(test1.elf ...)` and `target_link_libraries(test1.elf PRIVATE udynlink)`.
  - Handle the extra define `OS_INCLUDE_STARTUP_INIT_MULTIPLE_RAM_SECTIONS` only for `_startup.c` (use `set_source_files_properties`).
  - Ensure linker script paths are resolved relative to `tests/qemu_host/ldscripts/`.
  - **Delete** `tests/qemu_host/Debug/` entirely.
- **Deliverable**: `cmake -B tests/build -S tests/qemu_host && cmake --build tests/build` produces `tests/build/test1.elf`. Also works via root: `cmake -B build -S . -DUDYNLINK_BUILD_TESTS=ON`.
- **Review**: Compare `nm` / `readelf` output of old `test1.elf` vs. new `test1.elf` to confirm identical symbols and sections.

### Task 4: Update `test_driver.py` for CMake and new QEMU lookup
- **Agent**: `agent` (Python script modification)
- **Files**: `tests/test_driver.py`, `tests/test.sh`
- **Details**:
  - Replace `make` invocations with `cmake -B ../build -S ../qemu_host -DUDYNLINK_BUILD_TESTS=ON` and `cmake --build ../build --target test1.elf`.
  - Keep the `cleaned` global logic, but adapt it to `cmake --build ../build --target clean` (or delete `tests/build/`).
  - Update QEMU command resolution to search PATH + env var `UDYNLINK_QEMU_BIN` + known relative paths.
  - Update `test.sh` to reference the new build directory and any changed paths.
- **Deliverable**: `python3 test_driver.py test-helloworld` passes with both `-O0` and `-Os`.
- **Review**: Run a single test and inspect output logs.

### Task 5: Update CI workflow
- **Agent**: `quick` (small YAML edit)
- **Files**: `.github/workflows/ci.yml`
- **Details**:
  - Remove `wget` and `tar` for xPack QEMU.
  - Add `cmake` to `apt-get install`.
  - Set `UDYNLINK_QEMU_BIN` or ensure `qemu-system-gnuarmeclipse` is in PATH (document expectation that runner image has it).
  - Keep existing test command `cd tests && python ./test_driver.py`.
- **Deliverable**: CI YAML is syntactically correct and logically consistent.
- **Review**: Eyeball diff; no execution needed until Task 7.

### Task 6: Update documentation
- **Agent**: `quick` (documentation updates)
- **Files**: `AGENTS.md`, `tests/codemap.md`, `README.md`
- **Details**:
  - Update `AGENTS.md` "Build & Test Commands" section to show CMake commands instead of Eclipse makefiles.
  - Update `tests/codemap.md` to describe CMake + platform abstraction instead of Eclipse makefiles.
  - Update `README.md` if it references Eclipse or `Debug/makefile`.
  - Document the `UDYNLINK_CC_PREFIX`, `--mcpu`, `UDYNLINK_PLATFORM`, and `UDYNLINK_BUILD_TESTS` options.
  - Update `.gitignore` to ignore `tests/build/` (and any new CMake artifacts) instead of `tests/qemu_host/Debug/` artifacts.
- **Deliverable**: All docs are consistent with the new build system.
- **Review**: Read updated docs and verify no stale references remain.

### Task 7: End-to-end validation
- **Agent**: `expert` (run full test suite, debug regressions)
- **Files**: Entire test suite
- **Details**:
  - Run `python3 test_driver.py` in `tests/` (no prefix argument = all tests).
  - Verify every test passes under both `-O0` and `-Os`.
  - If any test fails, investigate whether it's a compiler flag divergence, a missing source file in CMake, or a linker script path issue.
- **Deliverable**: Output shows `Total: N, Failed: 0`.
- **Review**: User-visible confirmation of success.

---

## Execution Order & Dependencies

```
Task 1  ──►  Task 4  (mkmodule prefix/CPU change is needed by test_driver)
Task 2  ──►  Task 3  (root CMake can be done before or parallel to test host)
Task 3  ──►  Task 4  (test_driver depends on test host CMake working)
Task 4  ──►  Task 7  (E2E validation depends on test_driver)
Task 5  ──►  Task 7  (CI can be updated in parallel with test_driver)
Task 6  ──►  Task 7  (Docs can be updated in parallel with test_driver)
```

Recommended sequence for dispatch:
1. **Batch 1** (parallel): Task 1 + Task 2 + Task 5 + Task 6
2. **Batch 2** (after Batch 1): Task 3
3. **Batch 3** (after Task 3): Task 4
4. **Batch 4** (after all above): Task 7

---

## Multi-Target / Multi-Platform Extensibility

### How to add a new Cortex-M target (e.g., Cortex-M0+, M7, M33)

1. **Update `mkmodule`**: Pass `--mcpu cortex-m0plus` (or whatever) when building modules for the new target. The linker script `code_before_data.ld` is already CPU-agnostic.
2. **Add a CMake platform file**: Create `cmake/platforms/<platform_name>.cmake` with the correct `-mcpu`, `-mfloat-abi`, HAL include paths, linker scripts, and defines for that MCU.
3. **Add HAL/CMSIS sources**: Place the vendor SDK under `tests/qemu_host/system/` (or a new `platforms/` directory if you later separate it). Update the source list in the test host `CMakeLists.txt` or make it conditional on `UDYNLINK_PLATFORM`.
4. **Update `test_driver.py`**: If QEMU supports the new board, add the appropriate `-machine` / `-cpu` args (or rely on env vars).

### Design choices that make this easy

| Decision | Rationale |
|----------|-----------|
| **Platform-specific logic isolated in `cmake/platforms/*.cmake`** | Adding a new board = new file, no edits to core library or test host CMake. |
| **`UDYNLINK_PLATFORM` cache variable** | Switching boards is a single CMake flag: `-DUDYNLINK_PLATFORM=nrf52840_dk`. |
| **Core library has no `-mcpu` flags** | It compiles for any ARM target; the consumer sets the architecture. |
| **`mkmodule` `--mcpu` flag** | Module compiler can target any Cortex-M without code changes. |
| **Test host `CMakeLists.txt` is standalone** | Can be copied/adapted for a real firmware project that consumes `udynlink`. |

---

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| CMake does not replicate exact Eclipse compiler/linker flags, causing runtime failures in QEMU. | Use `cmake --build tests/build --verbose` to diff compile/link lines against old `subdir.mk` / `makefile`. Spot-check with `readelf -h` and `objdump -h`. |
| Missing a source file in CMake (STM32 HAL has many files). | Copy the file list verbatim from `sources.mk` + all `subdir.mk` files. Cross-check with `find tests/qemu_host -name '*.c'`. |
| `_startup.c` needs an extra define not applied to other files. | Use `set_source_files_properties(../system/src/newlib/_startup.c PROPERTIES COMPILE_DEFINITIONS OS_INCLUDE_STARTUP_INIT_MULTIPLE_RAM_SECTIONS)`. |
| xPack QEMU is not present in a new CI runner image. | Document the required runner image/tag. Keep a fallback env var `UDYNLINK_QEMU_BIN` so local/CI paths are configurable. |
| `test_driver.py` `cleaned` logic breaks with CMake. | Replace with `shutil.rmtree(build_dir, ignore_errors=True)` before each test, or rely on CMake dependency tracking to rebuild `test_qemu.c` automatically. |
| Future platform files duplicate common Cortex-M logic. | Keep a `cmake/platforms/common.cmake` for shared flags (e.g., `-ffunction-sections`, `-fdata-sections`, `-std=gnu11`) that each platform includes. |

---

## Notes for Implementation Agents

- **Before editing `test_driver.py`**, re-read it carefully. It currently assumes `../qemu_host/Debug` and hardcodes a QEMU path that includes `./xpack-qemu-arm-7.2.5-1/bin/qemu-system-gnuarmeclipse`. The new logic should:
  1. Resolve QEMU binary via env var → PATH lookup → fallback relative path.
  2. Use `../build` as the unified build directory.
- **Before deleting `tests/qemu_host/Debug/`**, verify that no non-generated files exist inside it. From exploration, it contains *only* Eclipse-generated makefiles and build artifacts (`.o`, `.d`, `.elf`), all of which are already in `.gitignore`.
- **Platform file design**: `cmake/platforms/stm32f429_discovery.cmake` should set **all** target-specific compile/link flags. The test host `CMakeLists.txt` should only declare sources and link to `udynlink`. This separation is critical for extensibility.
- **`mkmodule` `--mcpu`**: Default to `cortex-m4` to preserve backward compatibility. Validate that the value is used in `compile_cmd`, `asm_cmd`, and `link_cmd`.
- **Root CMake**: The `option(UDYNLINK_BUILD_TESTS ...)` is OFF by default so that a downstream project doing `add_subdirectory(udynlink)` doesn't accidentally build the STM32 HAL and all its source files.
