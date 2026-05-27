cmake_minimum_required(VERSION 3.16)

# Set the ARM cross-compiler before any project() call in subdirectories
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER arm-none-eabi-gcc CACHE STRING "C compiler" FORCE)
    set(CMAKE_C_COMPILER_WORKS TRUE CACHE BOOL "" FORCE)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER arm-none-eabi-g++ CACHE STRING "C++ compiler" FORCE)
    set(CMAKE_CXX_COMPILER_WORKS TRUE CACHE BOOL "" FORCE)
endif()

project(udynlink_test_host LANGUAGES C CXX ASM)

# Allow overriding paths for parallel test execution (each test gets its own
# staging directory for test_qemu.c and generated module headers).
set(UDYNLINK_TEST_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src" CACHE PATH "Directory containing test_qemu.c and generated module headers")
set(UDYNLINK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to udynlink repository root")

# Standalone safety: grab the library if not already a target
if(NOT TARGET udynlink)
    # Prevent the root CMakeLists.txt from recursing back into tests/qemu_host
    # when it sees UDYNLINK_BUILD_TESTS=ON (inherited by default in add_subdirectory)
    set(UDYNLINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        ${UDYNLINK_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/udynlink
    )
endif()

# Platform selection
set(UDYNLINK_PLATFORM "stm32f429_discovery" CACHE STRING "Target platform for test host")
include(${CMAKE_CURRENT_SOURCE_DIR}/../platforms/${UDYNLINK_PLATFORM}/platform.cmake)

# Common sources (present for all platforms)
# Prefer test_qemu.cpp if it exists (C++ ergonomics tests), else test_qemu.c
if(EXISTS ${UDYNLINK_TEST_SRC_DIR}/test_qemu.cpp)
    set(TEST_QEMU_SOURCE ${UDYNLINK_TEST_SRC_DIR}/test_qemu.cpp)
else()
    set(TEST_QEMU_SOURCE ${UDYNLINK_TEST_SRC_DIR}/test_qemu.c)
endif()

set(TEST1_COMMON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${TEST_QEMU_SOURCE}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/test_utils.c
)

# Platform-specific sources (defined by platform.cmake)
if(NOT DEFINED UDYNLINK_PLATFORM_SOURCES)
    message(FATAL_ERROR "UDYNLINK_PLATFORM_SOURCES not defined by platform '${UDYNLINK_PLATFORM}'")
endif()

add_executable(test1.elf ${TEST1_COMMON_SOURCES} ${UDYNLINK_PLATFORM_SOURCES})

# --- Compile options and defines ---
target_compile_options(test1.elf PRIVATE
    ${UDYNLINK_PLATFORM_CFLAGS}
    ${UDYNLINK_PLATFORM_COMPILE_OPTIONS}
)

target_compile_definitions(test1.elf PRIVATE
    ${UDYNLINK_PLATFORM_DEFINES}
    UDYNLINK_MAX_HANDLES=8
)

# Allow overriding the compile-time debug level at build time
if(DEFINED UDYNLINK_TEST_DEBUG_LEVEL)
    target_compile_definitions(test1.elf PRIVATE UDYNLINK_DEBUG_LEVEL=${UDYNLINK_TEST_DEBUG_LEVEL})
endif()

# Ensure the udynlink library is compiled with the same core flags
if(TARGET udynlink)
    target_compile_options(udynlink PRIVATE
        ${UDYNLINK_PLATFORM_CFLAGS}
        ${UDYNLINK_PLATFORM_COMPILE_OPTIONS}
    )
    target_compile_definitions(udynlink PRIVATE
        ${UDYNLINK_PLATFORM_DEFINES}
        UDYNLINK_MAX_HANDLES=8
    )
endif()

# --- Include directories ---
target_include_directories(test1.elf PRIVATE
    ${UDYNLINK_PLATFORM_INCLUDE_DIRS}
    ${UDYNLINK_TEST_SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Per-file special flags are set directly by the included platform.cmake

# --- Link settings ---
target_link_options(test1.elf PRIVATE
    ${UDYNLINK_PLATFORM_CFLAGS}
    ${UDYNLINK_PLATFORM_LINK_OPTIONS}
)

target_link_directories(test1.elf PRIVATE
    ${UDYNLINK_PLATFORM_LINK_DIRS}
)

target_link_libraries(test1.elf PRIVATE udynlink)

# --- Standard C++ if any C++ sources ---
if(UDYNLINK_PLATFORM_HAS_CXX)
    set_target_properties(test1.elf PROPERTIES LINKER_LANGUAGE CXX)
endif()
