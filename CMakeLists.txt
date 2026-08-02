cmake_minimum_required(VERSION 3.16)

# Pre-set the ARM cross-compiler when building tests, before project() triggers detection
if(UDYNLINK_BUILD_TESTS)
    if(NOT CMAKE_C_COMPILER)
        set(CMAKE_C_COMPILER arm-none-eabi-gcc CACHE STRING "C compiler" FORCE)
        set(CMAKE_C_COMPILER_WORKS TRUE CACHE BOOL "" FORCE)
    endif()
    if(NOT CMAKE_CXX_COMPILER)
        set(CMAKE_CXX_COMPILER arm-none-eabi-g++ CACHE STRING "C++ compiler" FORCE)
        set(CMAKE_CXX_COMPILER_WORKS TRUE CACHE BOOL "" FORCE)
    endif()
endif()

project(udynlink VERSION 0.1.0 LANGUAGES C)

# Core library target
add_library(udynlink STATIC udynlink/udynlink.c)

target_include_directories(udynlink PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/udynlink>
    $<INSTALL_INTERFACE:include>
)

# Standalone thunk pool (optional; link if needed)
add_library(udynlink_thunk STATIC udynlink/udynlink_thunk.c)
target_link_libraries(udynlink_thunk PUBLIC udynlink)
target_include_directories(udynlink_thunk PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/udynlink>
    $<INSTALL_INTERFACE:include>
)

# Standalone dependency system (optional; link if needed)
add_library(udynlink_deps STATIC udynlink/udynlink_deps.c)
target_link_libraries(udynlink_deps PUBLIC udynlink_thunk)
target_include_directories(udynlink_deps PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/udynlink>
    $<INSTALL_INTERFACE:include>
)

# Scripts directory: available to both FetchContent and installed consumers
set(udynlink_SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/scripts" CACHE PATH
    "Path to udynlink scripts (mkhostsyms, mkmodule, etc.)"
)

# Public headers directory (udynlink_deps_api.h, udynlink.h, ...): used by
# udynlink_add_module() to pass -I to mkmodule so module sources can include
# the headers. Also resolved by udynlinkConfig.cmake for installed consumers.
set(udynlink_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/udynlink" CACHE PATH
    "Path to udynlink public headers"
)

# Install support
include(GNUInstallDirs)
install(TARGETS udynlink
    EXPORT udynlinkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT udynlinkTargets
    FILE udynlinkTargets.cmake
    NAMESPACE udynlink::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/udynlink
)

install(FILES
    udynlink/udynlink.h
    udynlink/udynlink_externals.h
    udynlink/udynlink_hash.h
    udynlink/udynlink_call.h
    udynlink/udynlink_thunk.h
    udynlink/udynlink.hpp
    udynlink/udynlink_deps.h
    udynlink/udynlink_deps_api.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/udynlink
)

install(TARGETS udynlink_thunk
    EXPORT udynlinkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(TARGETS udynlink_deps
    EXPORT udynlinkTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Package config file
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/udynlinkConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/udynlinkConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/udynlink
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/udynlinkConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/udynlinkConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/udynlinkConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/udynlink
)

# Make helper functions available at build time (FetchContent consumers)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/udynlinkGenerateHostSyms.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/udynlinkAddModule.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/udynlinkAddHostTest.cmake")

# Install scripts and CMake helpers
install(DIRECTORY scripts/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/udynlink/scripts
    PATTERN "__pycache__" EXCLUDE
    PATTERN "codemap.md" EXCLUDE
)
install(FILES
    cmake/udynlinkGenerateHostSyms.cmake
    cmake/udynlinkAddModule.cmake
    cmake/udynlinkAddHostTest.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/udynlink
)

# Tests option (OFF by default so consumers don't pull in HAL)
option(UDYNLINK_BUILD_TESTS "Build QEMU test host firmware" OFF)
if(UDYNLINK_BUILD_TESTS)
    add_subdirectory(tests/qemu_host)
endif()

if(UDYNLINK_BUILD_FUZZERS)
    enable_testing()
    add_subdirectory(tests/fuzz)
endif()
