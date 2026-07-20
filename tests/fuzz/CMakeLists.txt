# Host-side sanitizer / libFuzzer harnesses for the udynlink loader.
#
# Compiles udynlink/udynlink.c natively (ARM Thumb module code is never
# executed — only the loader's C logic runs on host). The r9-setting
# UDYNLINK_PREPARE_CALL macro in udynlink.h is guarded by #ifndef, so passing
# -DUDYNLINK_PREPARE_CALL(p_mod)=((void)(p_mod)) makes it a no-op here. The
# fuzzer never calls udynlink_cpp_init, so the empty body is safe.

set(_udynlink_loader_src ${CMAKE_SOURCE_DIR}/udynlink/udynlink.c)
set(_udynlink_fuzz_inc ${CMAKE_SOURCE_DIR}/udynlink)
set(_udynlink_prepare_call_override
    "-DUDYNLINK_PREPARE_CALL(p_mod)=((void)(p_mod))")

# libFuzzer target — clang only (gcc has no -fsanitize=fuzzer).
if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
    add_executable(udynlink_fuzz_load
        fuzz_load_module.c
        fuzz_harness.c
        fuzz_host_externals.c
        ${_udynlink_loader_src})
    target_include_directories(udynlink_fuzz_load PRIVATE ${_udynlink_fuzz_inc})
    target_compile_options(udynlink_fuzz_load PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer -g
        ${_udynlink_prepare_call_override})
    target_link_options(udynlink_fuzz_load PRIVATE
        -fsanitize=fuzzer,address,undefined)
else()
    message(STATUS "udynlink_fuzz_load skipped: requires clang (found ${CMAKE_C_COMPILER_ID})")
endif()

# (enable_testing lives at top-level so ctest picks up registered tests from
# the build-tree root, not just this subdirectory.)
# Sanitizer-only regression gate — gcc or clang, no libFuzzer.
add_executable(udynlink_san_load
    san_load_module.c
    fuzz_harness.c
    fuzz_host_externals.c
    ${_udynlink_loader_src})
target_include_directories(udynlink_san_load PRIVATE ${_udynlink_fuzz_inc})
target_compile_options(udynlink_san_load PRIVATE
    -fsanitize=address,undefined
    -fno-omit-frame-pointer -g -O1
    ${_udynlink_prepare_call_override})
target_link_options(udynlink_san_load PRIVATE -fsanitize=address,undefined)
add_test(NAME udynlink_san_load COMMAND udynlink_san_load
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
