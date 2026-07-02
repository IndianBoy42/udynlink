#[=======================================================================[.rst:
udynlink_add_host_test
----------------------

Build a native host test binary that links udynlink module sources
against mock implementations of the host symbols they consume.

Host tests do NOT exercise the udynlink loader (LOT/r9 relocations,
prologue wrappers, ABI checks). They test module *logic* by compiling
the same C/C++ sources the module is built from, together with mock
implementations of host-provided symbols, into a plain host binary.

See ``docs/host-testing.md`` for the full guide, including mocking
patterns, cross-module tests, C++ constructor handling, and fidelity
gaps between host and target behavior.

.. command:: udynlink_add_host_test

  .. code-block:: cmake

    udynlink_add_host_test(<name>
      SOURCES <src...>            # module + any non-mock, non-test sources
      [MOCK_SOURCES <src...>]     # mock implementations of host symbols
      [TEST_SOURCES <src...>]     # test driver(s) containing main()
      [INCLUDE_DIRS <dir...>]     # extra include directories
      [BUILD_FLAGS <flags>]       # extra compiler flags
      [LINK_LIBS <lib...>]        # extra link libraries
      [CXX_STANDARD <11|14|17|20>] # C++ standard (default: 17)
      [WORKING_DIRECTORY <dir>]   # CTest working directory
    )

Creates an executable target ``<name>`` registered with CTest. C++
sources are compiled with the same restricted subset as udynlink
modules (``-fno-exceptions -fno-rtti -fno-use-cxa-atexit``) so host
tests cannot accidentally rely on features unavailable on target.

The target is a standard CMake executable; extend it after the call
with ``target_compile_options``, ``target_link_libraries``, etc.

Example:

.. code-block:: cmake

  include(FetchContent)
  FetchContent_Declare(udynlink GIT_REPOSITORY ... GIT_TAG ...)
  FetchContent_MakeAvailable(udynlink)

  enable_testing()

  udynlink_add_host_test(test_regulate
    SOURCES      src/mod_example.c
    MOCK_SOURCES  mocks/mock_host.c
    TEST_SOURCES  tests/test_regulate.c)
#]=======================================================================]

function(udynlink_add_host_test name)
  set(_options)
  set(_oneval CXX_STANDARD WORKING_DIRECTORY BUILD_FLAGS)
  set(_multival SOURCES MOCK_SOURCES TEST_SOURCES INCLUDE_DIRS LINK_LIBS)
  cmake_parse_arguments(ARG "${_options}" "${_oneval}" "${_multival}" ${ARGN})

  set(_all_sources ${ARG_SOURCES} ${ARG_MOCK_SOURCES} ${ARG_TEST_SOURCES})
  if(NOT _all_sources)
    message(FATAL_ERROR "udynlink_add_host_test: no SOURCES, MOCK_SOURCES, or TEST_SOURCES given")
  endif()

  if(NOT ARG_CXX_STANDARD)
    set(ARG_CXX_STANDARD 17)
  endif()

  add_executable(${name} ${_all_sources})

  # Match the module toolchain's restricted C++ subset so host tests
  # cannot use exceptions/RTTI/atexit that the module cannot use on target.
  target_compile_options(${name} PRIVATE
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-use-cxa-atexit>)

  if(ARG_INCLUDE_DIRS)
    target_include_directories(${name} PRIVATE ${ARG_INCLUDE_DIRS})
  endif()
  if(ARG_BUILD_FLAGS)
    target_compile_options(${name} PRIVATE ${ARG_BUILD_FLAGS})
  endif()
  if(ARG_LINK_LIBS)
    target_link_libraries(${name} PRIVATE ${ARG_LINK_LIBS})
  endif()

  set_target_properties(${name} PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    CXX_STANDARD ${ARG_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON)

  if(NOT ARG_WORKING_DIRECTORY)
    set(ARG_WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endif()

  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${ARG_WORKING_DIRECTORY}")
endfunction()
