# Justfile for udynlink - Micro Dynamic Linker for ARM Cortex-M
# https://github.com/eh2k/udynlink
#
# Requires: just (https://github.com/casey/just)
#           arm-none-eabi-gcc, cmake, python3, uv, qemu-system-arm

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

# QEMU binaries
qemu_bin := env_var_or_default("UDYNLINK_QEMU_BIN", "qemu-system-arm")
qemu_legacy := env_var_or_default("UDYNLINK_QEMU_BIN", "qemu-system-gnuarmeclipse")

# Default platform
platform := env_var_or_default("UDYNLINK_PLATFORM", "stm32f429_discovery")

# Module target
module_target := env_var_or_default("UDYNLINK_MODULE_TARGET", "cortex-m4")

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

# Build the core library + tests in-tree
build-all:
    cmake -B {{build_dir}} -S . -DUDYNLINK_BUILD_TESTS=ON
    cmake --build {{build_dir}} --target test1.elf

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
    python3 test_driver.py

# Run a specific test on STM32F429
test-f429-single test_name:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=stm32f429_discovery \
    UDYNLINK_QEMU_BIN={{qemu_legacy}} \
    UDYNLINK_QEMU_MACHINE=STM32F429I-Discovery \
    python3 test_driver.py {{test_name}}

# =============================================================================
# Test Commands - MPS2-AN386 (Mainline QEMU 9.x)
# =============================================================================

# Run all tests on MPS2-AN386 (mainline QEMU, Cortex-M4)
test-mps2:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=mps2_an386 \
    UDYNLINK_QEMU_BIN=qemu-system-arm \
    UDYNLINK_QEMU_MACHINE=mps2-an386 \
    UDYNLINK_QEMU_CPU=cortex-m4 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=120 \
    python3 test_driver.py

# Run a specific test on MPS2-AN386
test-mps2-single test_name:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=mps2_an386 \
    UDYNLINK_QEMU_BIN=qemu-system-arm \
    UDYNLINK_QEMU_MACHINE=mps2-an386 \
    UDYNLINK_QEMU_CPU=cortex-m4 \
    UDYNLINK_QEMU_EXTRA_FLAGS="-semihosting" \
    UDYNLINK_QEMU_TIMEOUT=120 \
    python3 test_driver.py {{test_name}}

# =============================================================================
# Test Commands - Other Platforms (Skeleton/Test Only)
# =============================================================================

# Run tests on STM32F103 (Cortex-M3) - requires qemu-system-gnuarmeclipse
test-f103:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=stm32f103_bluepill \
    UDYNLINK_QEMU_BIN={{qemu_legacy}} \
    UDYNLINK_QEMU_MACHINE=NUCLEO-F103RB \
    python3 test_driver.py

# Run tests on STM32F051 (Cortex-M0) - requires qemu-system-gnuarmeclipse
test-f051:
    #!/usr/bin/env bash
    cd {{tests_dir}}
    UDYNLINK_PLATFORM=stm32f051_discovery \
    UDYNLINK_QEMU_BIN={{qemu_legacy}} \
    UDYNLINK_QEMU_MACHINE=STM32F0-Discovery \
    python3 test_driver.py

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
# Module Compilation
# =============================================================================

# Compile a loadable module for the default target (cortex-m4)
module source_file *args="":
    cd {{scripts_dir}} && uv run python3 mkmodule \
        --target {{module_target}} \
        {{args}} \
        {{source_file}}

# Compile a module for a specific target
module-for target source_file *args="":
    cd {{scripts_dir}} && uv run python3 mkmodule \
        --target {{target}} \
        {{args}} \
        {{source_file}}

# Compile a module with C header generation
module-header source_file header_path *args="":
    cd {{scripts_dir}} && uv run python3 mkmodule \
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
        uv run python3 ../../scripts/mkmodule \
            --target $target \
            --bin-name /tmp/mod_hello_${target}.bin \
            hello.c || echo "FAILED: $target"
    done

# =============================================================================
# Target Database Queries
# =============================================================================

# Show supported targets and their properties
targets:
    @cd {{scripts_dir}} && uv run python3 list_targets.py

# Show detailed info for a specific target
target-info target=module_target:
    @cd {{scripts_dir}} && uv run python3 list_targets.py {{target}}

# =============================================================================
# Development & Debug
# =============================================================================

# Generate codemap (requires cargo + cartography skill)
codemap:
    @echo "See .opencode/skills/cartography/SKILL.md for codemap generation"

# Run a quick compile check on the core library
compile-check:
    arm-none-eabi-gcc -c -mcpu=cortex-m4 -mthumb -I. \
        -DUDYNLINK_MAX_HANDLES=8 \
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

# Run the full CI test suite (STM32F429 + MPS2-AN386)
ci:
    just test-f429
    just test-mps2

# Validate all targets can compile
ci-validate-targets:
    just validate-all-targets

# Quick CI check (compile only, no QEMU)
ci-quick:
    just build-lib
    just compile-check
    just validate-all-targets

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
    @echo "TEST (MPS2-AN386 - mainline QEMU 9.x):"
    @echo "  just test-mps2              - Run all tests on MPS2-AN386"
    @echo "  just test-mps2-single NAME  - Run specific test"
    @echo ""
    @echo "TEST (Other platforms):"
    @echo "  just test-f103              - STM32F103 (M3)"
    @echo "  just test-f051              - STM32F051 (M0)"
    @echo ""
    @echo "QEMU (Manual debugging):"
    @echo "  just qemu-mps2              - Run test1.elf on MPS2-AN386"
    @echo "  just qemu-mps2-gdb [PORT]   - Run with GDB server"
    @echo "  just qemu-f429              - Run test1.elf on STM32F429"
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
    @echo "  just ci                      - Full CI suite"
    @echo "  just ci-quick                - Compile-only checks"
    @echo ""
    @echo "ENVIRONMENT VARIABLES:"
    @echo "  UDYNLINK_QEMU_BIN           - QEMU binary path"
    @echo "  UDYNLINK_PLATFORM           - Target platform (default: stm32f429_discovery)"
    @echo "  UDYNLINK_MODULE_TARGET      - Module CPU target (default: cortex-m4)"
    @echo "  UDYNLINK_CMAKE_FLAGS        - Extra CMake flags"
