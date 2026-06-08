# Justfile for udynlink - Micro Dynamic Linker for ARM Cortex-M
# https://github.com/eh2k/udynlink
#
# Requires: just (https://github.com/casey/just)
#           arm-none-eabi-gcc, cmake, python3, uv, qemu-system-arm
#
# NOTE: All test commands below use the correct QEMU flags and module targets
# for their platform. Use `just` for all test running (do not run test_driver.py
# manually with ad-hoc env vars).

# Default recipe - show help
_default:
    @just --list

# =============================================================================
# Variables
# =============================================================================

# Build directories
build_dir := "build"
tests_build_dir := "tests/build"
scripts_dir := "scripts"
tests_dir := "tests"

# Repository root for absolute paths
repo_root := justfile_directory()

# xPack QEMU versions
qemu_xpack_version := "9.2.4-1"
qemu_legacy_version := "7.2.5-1"

# Paths to locally installed xPack QEMU directories
qemu_xpack_dir := "tests/xpack-qemu-arm-" + qemu_xpack_version
qemu_legacy_dir := "tests/xpack-qemu-arm-" + qemu_legacy_version

# QEMU binaries — xPack releases ONLY.  System QEMU is not supported because
# ABI differences (semihosting, machine models, CPU flags) cause subtle test
# failures.  Download the required xPack release with `just setup-qemu` or
# `just setup-qemu-legacy`.
#
# Mainline qemu-system-arm: 9.2.4-1 (required by MPS2-ANxxx / Olimex tests).
# Legacy qemu-system-gnuarmeclipse: 7.2.5-1 (last release with this binary).

qemu_bin := repo_root / qemu_xpack_dir / "bin/qemu-system-arm"

qemu_legacy := if path_exists(qemu_legacy_dir / "bin/qemu-system-gnuarmeclipse") == "true" {
    repo_root / qemu_legacy_dir / "bin/qemu-system-gnuarmeclipse"
} else {
    env_var_or_default("UDYNLINK_QEMU_LEGACY_BIN", "qemu-system-gnuarmeclipse")
}

# Default platform
platform := env_var_or_default("UDYNLINK_PLATFORM", "stm32f429_discovery")

# Module target
module_target := env_var_or_default("UDYNLINK_MODULE_TARGET", "cortex-m4")

# Python command (uses uv to ensure dependencies are available)
python_cmd := "uv run python3"

# CMake flags
cmake_flags := env_var_or_default("UDYNLINK_CMAKE_FLAGS", "")

# =============================================================================
# Build Commands
# =============================================================================

# Build the core udynlink library
build-lib:
    cmake -B {{build_dir}} -S .
    cmake --build {{build_dir}}

# Build tests for a specific platform (default: stm32f429_discovery)
build-tests platform=platform:
    cmake -B {{tests_build_dir}} -S {{tests_dir}}/qemu_host \
        -DUDYNLINK_BUILD_TESTS=ON \
        -DUDYNLINK_PLATFORM={{platform}} \
        {{cmake_flags}}
    cmake --build {{tests_build_dir}} --target test1.elf

# Build tests for all available platforms
build-tests-all:
    just build-tests stm32f429_discovery
    just build-tests mps2_an386
    just build-tests mps2_an385
    just build-tests mps2_an500
    just build-tests mps2_an505
    just build-tests microbit
    just build-tests olimex_stm32_h405
    just build-tests stm32f103_bluepill
    just build-tests stm32f051_discovery

# Build the core library + tests in-tree
build-all:
    cmake -B {{build_dir}} -S . -DUDYNLINK_BUILD_TESTS=ON
    cmake --build {{build_dir}} --target test1.elf

[parallel]
test-all: ci test-f429 test-f103 test-f051


# =============================================================================
# Test Commands - STM32F429 (Legacy xPack QEMU)
# =============================================================================

# Run all tests on STM32F429 (requires qemu-system-gnuarmeclipse)
test-f429:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=stm32f429_discovery \
    UDYNLINK_QEMU_BIN={{qemu_legacy}} \
    UDYNLINK_QEMU_MACHINE=STM32F429I-Discovery \
    {{python_cmd}} test_driver.py

# Run a specific test on STM32F429
test-f429-single test_name:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=stm32f429_discovery \
    UDYNLINK_QEMU_BIN={{qemu_legacy}} \
    UDYNLINK_QEMU_MACHINE=STM32F429I-Discovery \
    {{python_cmd}} test_driver.py {{test_name}}

# =============================================================================
# Test Commands - Mainline QEMU (M4/M3/M7/M33/M4F)
# =============================================================================

# Run all tests on MPS2-AN386 (mainline QEMU, Cortex-M4)
test-mps2:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=mps2_an386 \
    UDYNLINK_QEMU_BIN={{qemu_bin}} \
    UDYNLINK_QEMU_MACHINE=mps2-an386 \
    UDYNLINK_QEMU_CPU=cortex-m4 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py

# Run a specific test on MPS2-AN386
test-mps2-single test_name:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=mps2_an386 \
    UDYNLINK_QEMU_BIN={{qemu_bin}} \
    UDYNLINK_QEMU_MACHINE=mps2-an386 \
    UDYNLINK_QEMU_CPU=cortex-m4 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py {{test_name}}

# Run tests on MPS2-AN385 (Cortex-M3) - mainline QEMU
test-an385:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_MODULE_TARGET=cortex-m3 \
    UDYNLINK_PLATFORM=mps2_an385 \
    UDYNLINK_QEMU_BIN={{qemu_bin}} \
    UDYNLINK_QEMU_MACHINE=mps2-an385 \
    UDYNLINK_QEMU_CPU=cortex-m3 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py

# Run tests on MPS2-AN500 (Cortex-M7) - mainline QEMU
test-an500:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_MODULE_TARGET=cortex-m7 \
    UDYNLINK_PLATFORM=mps2_an500 \
    UDYNLINK_QEMU_BIN={{qemu_bin}} \
    UDYNLINK_QEMU_MACHINE=mps2-an500 \
    UDYNLINK_QEMU_CPU=cortex-m7 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py

# Run tests on MPS2-AN505 (Cortex-M33) - mainline QEMU
test-an505:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_MODULE_TARGET=cortex-m33 \
    UDYNLINK_PLATFORM=mps2_an505 \
    UDYNLINK_QEMU_BIN={{qemu_bin}} \
    UDYNLINK_QEMU_MACHINE=mps2-an505 \
    UDYNLINK_QEMU_CPU=cortex-m33 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py

# Run tests on Olimex STM32-H405 (Cortex-M4F hard-float) - mainline QEMU
test-h405:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_MODULE_TARGET=cortex-m4f \
    UDYNLINK_PLATFORM=olimex_stm32_h405 \
    UDYNLINK_QEMU_BIN={{qemu_bin}} \
    UDYNLINK_QEMU_MACHINE=olimex-stm32-h405 \
    UDYNLINK_QEMU_CPU=cortex-m4 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py

# Run tests on micro:bit (Cortex-M0) - mainline QEMU
# NOTE: Currently broken. QEMU microbit machine does not support `-kernel` ELF
# loading at 0x00000000. Needs `-device loader,file=...,addr=0x0` with raw binary.
test-microbit:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_MODULE_TARGET=cortex-m0 \
    UDYNLINK_PLATFORM=microbit \
    UDYNLINK_QEMU_BIN={{qemu_bin}} \
    UDYNLINK_QEMU_MACHINE=microbit \
    UDYNLINK_QEMU_CPU=cortex-m0 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=30 \
    {{python_cmd}} test_driver.py

# =============================================================================
# Test Commands - Legacy QEMU (M3/M0)
# =============================================================================

# Run tests on STM32F103 (Cortex-M3) - requires qemu-system-gnuarmeclipse
test-f103:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=stm32f103_bluepill \
    UDYNLINK_QEMU_BIN={{qemu_legacy}} \
    UDYNLINK_QEMU_MACHINE=NUCLEO-F103RB \
    {{python_cmd}} test_driver.py

# Run tests on STM32F051 (Cortex-M0) - requires qemu-system-gnuarmeclipse
test-f051:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=stm32f051_discovery \
    UDYNLINK_QEMU_BIN={{qemu_legacy}} \
    UDYNLINK_QEMU_MACHINE=STM32F0-Discovery \
    {{python_cmd}} test_driver.py

# =============================================================================
# Manual QEMU Commands (for debugging individual tests)
# =============================================================================

# Run a manually built test1.elf on MPS2-AN386 with GDB server
qemu-mps2-gdb port="1234":
    {{qemu_bin}} -machine mps2-an386 -cpu cortex-m4 \
        -kernel {{tests_build_dir}}/test1.elf \
        -nographic -semihosting -gdb tcp::{{port}} -S

# Run a manually built test1.elf on MPS2-AN386 (no GDB)
qemu-mps2:
    {{qemu_bin}} -machine mps2-an386 -cpu cortex-m4 \
        -kernel {{tests_build_dir}}/test1.elf \
        -nographic -semihosting

# Run a manually built test1.elf on STM32F429 (legacy QEMU)
qemu-f429:
    {{qemu_legacy}} -board STM32F429I-Discovery \
        -image {{tests_build_dir}}/test1.elf -nographic

# =============================================================================
# QEMU Setup & Status
# =============================================================================

# Download and extract the latest xPack QEMU into tests/ (mainline qemu-system-arm)
setup-qemu:
    #!/usr/bin/env bash
    set -euo pipefail
    XPACK_VER="{{qemu_xpack_version}}"
    XPACK_DIR="tests/xpack-qemu-arm-${XPACK_VER}"

    if [ -d "${XPACK_DIR}" ]; then
        echo "xPack QEMU ${XPACK_VER} already present at ${XPACK_DIR}"
        echo "Binaries:"
        ls -la "${XPACK_DIR}/bin/" | grep qemu || true
        exit 0
    fi

    ARCH=$(uname -m)
    case "$ARCH" in
        x86_64)  ARCH_TAG="linux-x64" ;;
        aarch64) ARCH_TAG="linux-arm64" ;;
        arm64)   ARCH_TAG="linux-arm64" ;;
        *) echo "Unsupported architecture: $ARCH"; echo "Please download manually from https://github.com/xpack-dev-tools/qemu-arm-xpack/releases"; exit 1 ;;
    esac

    URL="https://github.com/xpack-dev-tools/qemu-arm-xpack/releases/download/v${XPACK_VER}/xpack-qemu-arm-${XPACK_VER}-${ARCH_TAG}.tar.gz"
    TARBALL="/tmp/xpack-qemu-arm-${XPACK_VER}-${ARCH_TAG}.tar.gz"

    echo "Downloading xPack QEMU ${XPACK_VER} for ${ARCH_TAG}..."
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "${TARBALL}" "${URL}"
    else
        wget -q -O "${TARBALL}" "${URL}"
    fi

    echo "Extracting to tests/..."
    mkdir -p tests
    tar -xzf "${TARBALL}" -C tests/
    rm -f "${TARBALL}"

    echo ""
    echo "xPack QEMU ${XPACK_VER} installed at ${XPACK_DIR}"
    echo "Binaries:"
    ls -la "${XPACK_DIR}/bin/" | grep qemu || true
    echo ""
    echo "The Justfile will automatically prefer these local binaries."
    echo "Override with UDYNLINK_QEMU_BIN or UDYNLINK_QEMU_LEGACY_BIN if needed."

# Download legacy xPack QEMU (qemu-system-gnuarmeclipse) into tests/
setup-qemu-legacy:
    #!/usr/bin/env bash
    set -euo pipefail
    XPACK_VER="{{qemu_legacy_version}}"
    XPACK_DIR="tests/xpack-qemu-arm-${XPACK_VER}"

    if [ -d "${XPACK_DIR}" ]; then
        echo "Legacy xPack QEMU ${XPACK_VER} already present at ${XPACK_DIR}"
        echo "Binaries:"
        ls -la "${XPACK_DIR}/bin/" | grep qemu || true
        exit 0
    fi

    ARCH=$(uname -m)
    case "$ARCH" in
        x86_64)  ARCH_TAG="linux-x64" ;;
        aarch64) ARCH_TAG="linux-arm64" ;;
        arm64)   ARCH_TAG="linux-arm64" ;;
        *) echo "Unsupported architecture: $ARCH"; echo "Please download manually from https://github.com/xpack-dev-tools/qemu-arm-xpack/releases"; exit 1 ;;
    esac

    URL="https://github.com/xpack-dev-tools/qemu-arm-xpack/releases/download/v${XPACK_VER}/xpack-qemu-arm-${XPACK_VER}-${ARCH_TAG}.tar.gz"
    TARBALL="/tmp/xpack-qemu-arm-${XPACK_VER}-${ARCH_TAG}.tar.gz"

    echo "Downloading legacy xPack QEMU ${XPACK_VER} for ${ARCH_TAG}..."
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "${TARBALL}" "${URL}"
    else
        wget -q -O "${TARBALL}" "${URL}"
    fi

    echo "Extracting to tests/..."
    mkdir -p tests
    tar -xzf "${TARBALL}" -C tests/
    rm -f "${TARBALL}"

    echo ""
    echo "Legacy xPack QEMU ${XPACK_VER} installed at ${XPACK_DIR}"
    echo "Binaries:"
    ls -la "${XPACK_DIR}/bin/" | grep qemu || true
    echo ""
    echo "The Justfile will automatically prefer this local binary for legacy tests."
    echo "Override with UDYNLINK_QEMU_LEGACY_BIN if needed."

# Show which QEMU binaries will be used
qemu-status:
    #!/usr/bin/env bash
    echo "=== QEMU Status ==="
    echo ""
    echo "Mainline QEMU (qemu-system-arm):"
    QEMU_BIN="{{qemu_bin}}"
    if command -v "$QEMU_BIN" >/dev/null 2>&1; then
        echo "  Path: $(command -v "$QEMU_BIN")"
        "$QEMU_BIN" --version 2>/dev/null | head -1 || echo "  (version check failed)"
    else
        echo "  NOT FOUND: $QEMU_BIN"
        echo "  Run 'just setup-qemu' to download, or install via your package manager"
    fi
    echo ""
    echo "Legacy QEMU (qemu-system-gnuarmeclipse):"
    QEMU_LEGACY="{{qemu_legacy}}"
    if command -v "$QEMU_LEGACY" >/dev/null 2>&1; then
        echo "  Path: $(command -v "$QEMU_LEGACY")"
        "$QEMU_LEGACY" --version 2>/dev/null | head -1 || echo "  (version check failed)"
    else
        echo "  NOT FOUND: $QEMU_LEGACY"
        echo "  Run 'just setup-qemu-legacy' to download"
    fi

# =============================================================================
# Module Compilation
# =============================================================================

# Compile a loadable module for the default target (cortex-m4)
module source_file *args="":
    cd {{scripts_dir}} &&     {{python_cmd}} mkmodule \
        --target {{module_target}} \
        {{args}} \
        {{source_file}}

# Compile a module for a specific target
module-for target source_file *args="":
    cd {{scripts_dir}} &&     {{python_cmd}} mkmodule \
        --target {{target}} \
        {{args}} \
        {{source_file}}

# Compile a module with C header generation
module-header source_file header_path *args="":
    cd {{scripts_dir}} &&     {{python_cmd}} mkmodule \
        --target {{module_target}} \
        --gen-c-header --header-path {{header_path}} \
        {{args}} \
        {{source_file}}

# Compile test-helloworld for all 9 supported targets (validation)
validate-all-targets:
    #!/usr/bin/env bash
    cd {{tests_dir}}/test-helloworld
    for target in cortex-m0 cortex-m0plus cortex-m3 cortex-m4 cortex-m4f cortex-m7 cortex-m33 cortex-m55 cortex-m85; do
        echo "=== Compiling for $target ==="
        {{python_cmd}} ../../scripts/mkmodule \
            --target $target \
            --bin-name /tmp/mod_hello_${target}.bin \
            hello.c || echo "FAILED: $target"
    done

# =============================================================================
# Target Database Queries
# =============================================================================

# Show supported targets and their properties
targets:
    @cd {{scripts_dir}} && {{python_cmd}} list_targets.py

# Show detailed info for a specific target
target-info target=module_target:
    @cd {{scripts_dir}} && {{python_cmd}} list_targets.py {{target}}

# =============================================================================
# Benchmarks
# =============================================================================

# Run the toolchain benchmark suite (compile+mkmodule profiling)
bench *args="":
    cd benchmarks && {{python_cmd}} bench_toolchain.py {{args}}

# Run benchmarks with all opt levels and JSON output
bench-full target=module_target:
    cd benchmarks && {{python_cmd}} bench_toolchain.py --target {{target}} \
        --json /tmp/udynlink_bench_results.json

# Run benchmarks for a single benchmark across all opt levels
bench-one name target=module_target:
    cd benchmarks && {{python_cmd}} bench_toolchain.py --target {{target}} --bench {{name}}

# List available benchmark modules
bench-list:
    cd benchmarks && {{python_cmd}} bench_toolchain.py --list

# =============================================================================
# Development & Debug
# =============================================================================

# Generate codemap (requires cargo + cartography skill)
codemap:
    @echo "See .opencode/skills/cartography/SKILL.md for codemap generation"

# Run a quick compile check on the core library
compile-check:
    arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -I. \
        udynlink/udynlink.c -o /tmp/udynlink_check.o
    @echo "Core library compiles successfully"

# Check code style (basic)
lint:
    #!/usr/bin/env bash
    echo "=== Checking C code ==="
    find udynlink tests -name '*.c' -o -name '*.h' | head -20
    echo "=== Checking Python code ==="
    find scripts -name '*.py' | head -20

# =============================================================================
# Clean Commands
# =============================================================================

# Clean build artifacts
clean:
    rm -rf {{build_dir}} {{tests_build_dir}} {{tests_dir}}/build_*
    rm -f {{tests_dir}}/qemu_host/src/test_qemu.c
    rm -f {{tests_dir}}/qemu_host/src/*_module_data.h
    rm -f temp/*

# Clean everything including generated files
clean-all: clean
    rm -rf .pytest_cache __pycache__ .mypy_cache
    find . -name '*.pyc' -delete
    find . -name '__pycache__' -delete
    find . -name '*_module_data.h' -delete

# =============================================================================
# CI Commands
# =============================================================================

# Run the full CI test suite (all platforms that currently pass on mainline QEMU)
[parallel]
ci: test-mps2 test-an385 test-an500 test-an505 test-h405

# Validate all targets can compile
ci-validate-targets:
    just validate-all-targets

# Quick CI check (compile only, no QEMU)
ci-quick:
    just build-lib
    just compile-check
    just validate-all-targets

# Run Python unit tests (mkhostsyms, etc.)
test-py:
    uv run --with pytest pytest tests/py -v -m "not integration"

# Run Python tests including integration tests (requires arm-none-eabi-gcc)
test-py-all:
    uv run --with pytest pytest tests/py -v

# =============================================================================
# Help
# =============================================================================

# Show detailed help with examples
help:
    @echo "udynlink Justfile - Available commands"
    @echo ""
    @echo "BUILD:"
    @echo "  just build-lib              - Build core library"
    @echo "  just build-tests [PLATFORM] - Build tests for platform"
    @echo "  just build-all              - Build library + tests"
    @echo ""
    @echo "TEST (STM32F429 - legacy QEMU):"
    @echo "  just test-f429              - Run all tests on STM32F429"
    @echo "  just test-f429-single NAME  - Run specific test"
    @echo ""
    @echo "TEST (Mainline QEMU - M4/M3/M7/M33/M4F):"
    @echo "  just test-mps2              - MPS2-AN386 (Cortex-M4)"
    @echo "  just test-mps2-single NAME  - Single test on MPS2-AN386"
    @echo "  just test-an385             - MPS2-AN385 (Cortex-M3)"
    @echo "  just test-an500             - MPS2-AN500 (Cortex-M7)"
    @echo "  just test-an505             - MPS2-AN505 (Cortex-M33)"
    @echo "  just test-h405              - Olimex STM32-H405 (Cortex-M4F hard-float)"
    @echo ""
    @echo "TEST (Mainline QEMU - BROKEN):"
    @echo "  just test-microbit          - BBC micro:bit (Cortex-M0) - needs raw binary loader"
    @echo ""
    @echo "TEST (Legacy QEMU - M3/M0):"
    @echo "  just test-f103              - STM32F103 (M3)"
    @echo "  just test-f051              - STM32F051 (M0)"
    @echo ""
    @echo "QEMU (Manual debugging):"
    @echo "  just qemu-mps2              - Run test1.elf on MPS2-AN386"
    @echo "  just qemu-mps2-gdb [PORT]   - Run with GDB server"
    @echo "  just qemu-f429              - Run test1.elf on STM32F429"
    @echo ""
    @echo "QEMU SETUP:"
    @echo "  just setup-qemu             - Download latest xPack QEMU (mainline) into tests/"
    @echo "  just setup-qemu-legacy      - Download legacy xPack QEMU (gnuarmeclipse) into tests/"
    @echo "  just qemu-status            - Show which QEMU binaries will be used"
    @echo ""
    @echo "MODULE COMPILATION:"
    @echo "  just module FILE [ARGS]       - Compile module for default target"
    @echo "  just module-for TARGET FILE   - Compile for specific target"
    @echo "  just module-header FILE PATH  - Compile + generate C header"
    @echo "  just validate-all-targets     - Compile hello.c for all 9 targets"
    @echo ""
    @echo "INFO:"
    @echo "  just targets                 - List supported targets"
    @echo "  just target-info [TARGET]    - Show target details"
    @echo ""
    @echo "CLEAN:"
    @echo "  just clean                   - Remove build artifacts"
    @echo "  just clean-all               - Deep clean"
    @echo ""
    @echo "CI:"
    @echo "  just ci                      - Full CI suite (M4/M3/M7/M33/M4F on mainline QEMU)"
    @echo "  just ci-quick                - Compile-only checks"
    @echo ""
    @echo "BENCHMARKS:"
    @echo "  just bench [ARGS]            - Run toolchain benchmarks (pass args to bench_toolchain.py)"
    @echo "  just bench-list              - List available benchmark modules"
    @echo "  just bench-one NAME          - Run a single benchmark across all opt levels"
    @echo "  just bench-full              - Full benchmark suite with JSON output"
    @echo ""
    @echo "ENVIRONMENT VARIABLES:"
    @echo "  UDYNLINK_QEMU_BIN           - Mainline QEMU binary path"
    @echo "  UDYNLINK_QEMU_LEGACY_BIN    - Legacy QEMU binary path (gnuarmeclipse)"
    @echo "  UDYNLINK_PLATFORM           - Target platform (default: stm32f429_discovery)"
    @echo "  UDYNLINK_MODULE_TARGET      - Module CPU target (default: cortex-m4)"
    @echo "  UDYNLINK_CMAKE_FLAGS        - Extra CMake flags"
    @echo ""
    @echo "NOTE: Always run tests through 'just'. Manual invocation of test_driver.py"
    @echo "      with ad-hoc env vars is not supported and will likely fail."
