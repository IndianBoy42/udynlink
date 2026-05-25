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

# Library target
add_library(udynlink STATIC udynlink/udynlink.c udynlink/udynlink_hash.c)

target_include_directories(udynlink PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/udynlink>
    $<INSTALL_INTERFACE:include>
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
    udynlink/udynlink.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/udynlink
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

# Tests option (OFF by default so consumers don't pull in HAL)
option(UDYNLINK_BUILD_TESTS "Build QEMU test host firmware" OFF)
if(UDYNLINK_BUILD_TESTS)
    add_subdirectory(tests/qemu_host)
endif()
