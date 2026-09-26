#[=======================================================================[.rst:
udynlink_add_module
-------------------

Build a loadable udynlink module from C/C++ sources via ``mkmodule``,
exposing it as a CMake target plus an INTERFACE library a host firmware
target links to consume the generated embedding header.

.. command:: udynlink_add_module

  .. code-block:: cmake

    udynlink_add_module(<name>
      SOURCES <src...>            # one or more C/C++ sources (required)
      [TARGET <target>]          # mkmodule --target (default: cortex-m4)
      [MCPU <cpu>]               # mkmodule --mcpu override
      [PUBLIC_SYMBOLS <a,b,...>] # mkmodule --public-symbols (comma list)
      [OPT_LEVEL <0|s|2|3|z>]    # -O (default: s)
      [MODULE_NAME <name>]       # --module-name (default: <name>)
      [BUILD_FLAGS <flags>]      # --build-flags (extra compiler flags)
      [INCLUDE_DIRS <dir...>]    # extra -I dirs for the module sources
      [SECTIONS <spec...>]       # --section NAME[:align=N][:flags=F1,F2] (repeatable);
                                 # raises the loader ABI to 3.1 automatically
      [TAG_ON_ALIGN]             # --tag-on-align (tag when a main section needs >4 alignment)
      [STRIP_HIDDEN_SYMS]        # --strip-hidden-syms
      [STRIP_NON_PUBLIC_SYMS]    # --strip-non-public-syms (pairs with PUBLIC_SYMBOLS)
      [STRIP_MANGLED_SYMS]       # --strip-mangled-syms
      [STRIP_WEAK_SYM_NAMES]     # --strip-weak-sym-names
      [MOD_VERSION <ver>]        # --mod-version (default: 1.0)
      [UDYNLINK_VERSION <ver>]   # --udynlink-version (default: mkmodule's own —
                                 #   3.0, or 3.1 when SECTIONS is used)
      [NO_PROLOGUE]              # --no-prologue
      [PC_REL]                   # --pc-rel
      [NO_LONG_CALLS]            # --no-long-calls
      [LTO]                      # --lto (link-time optimization)
      [DISASM]                   # --disasm
      [WORKDIR <dir>]            # mkmodule --workdir (default:
                                 #   <build>/CMakeFiles/<name>.mkmodule)
      [OUTPUT_DIR <dir>]         # where the .bin is written (default: ${CMAKE_CURRENT_BINARY_DIR})
      [GENERATE_HEADER]          # also emit <name>_module_data.h
      [IMPORTS_HEADER]           # also emit <name>_imports.h (the module's symbol contract)
      [HEADER_OUTPUT_DIR <dir>]  # header dir (default: OUTPUT_DIR)
      [DEPENDS <dep...>]         # extra build-graph deps (targets or files)
    )

Creates:
  - custom target ``<name>`` producing ``${OUTPUT_DIR}/<name>.bin``
  - ``udynlink::module::<name>`` INTERFACE library that builds ``<name>``
    and (with GENERATE_HEADER) exports HEADER_OUTPUT_DIR as an include dir.

Consumer in one line:

.. code-block:: cmake

  include(FetchContent)
  FetchContent_Declare(udynlink GIT_REPOSITORY ... GIT_TAG ...)
  FetchContent_MakeAvailable(udynlink)

  udynlink_add_module(hello SOURCES src/hello.c GENERATE_HEADER)
  add_executable(firmware main.c)
  target_link_libraries(firmware PRIVATE udynlink::module::hello udynlink::udynlink)
#]=======================================================================]

function(udynlink_add_module name)
  set(_options NO_PROLOGUE PC_REL NO_LONG_CALLS LTO DISASM GENERATE_HEADER
              IMPORTS_HEADER TAG_ON_ALIGN
              STRIP_HIDDEN_SYMS STRIP_NON_PUBLIC_SYMS STRIP_MANGLED_SYMS
              STRIP_WEAK_SYM_NAMES)
  set(_oneval TARGET MCPU PUBLIC_SYMBOLS OPT_LEVEL MODULE_NAME BUILD_FLAGS
              MOD_VERSION UDYNLINK_VERSION OUTPUT_DIR HEADER_OUTPUT_DIR WORKDIR)
  set(_multival SOURCES DEPENDS INCLUDE_DIRS SECTIONS)
  cmake_parse_arguments(ARG "${_options}" "${_oneval}" "${_multival}" ${ARGN})

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "udynlink_add_module: SOURCES is required")
  endif()

  if(NOT ARG_TARGET)
    set(ARG_TARGET "cortex-m4")
  endif()
  if(NOT ARG_OPT_LEVEL)
    set(ARG_OPT_LEVEL "s")
  endif()
  if(NOT ARG_MOD_VERSION)
    set(ARG_MOD_VERSION "1.0")
  endif()
  # UDYNLINK_VERSION is left unset unless the caller asks for one: mkmodule's
  # own default is 3.0, and a SECTIONS build has to raise it to 3.1 — an
  # explicitly passed 3.0 together with --section is rejected there.
  if(NOT ARG_OUTPUT_DIR)
    set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  if(NOT ARG_HEADER_OUTPUT_DIR)
    set(ARG_HEADER_OUTPUT_DIR "${ARG_OUTPUT_DIR}")
  endif()
  if(NOT ARG_MODULE_NAME)
    set(ARG_MODULE_NAME "${name}")
  endif()

  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  if(NOT udynlink_SCRIPTS_DIR OR NOT EXISTS "${udynlink_SCRIPTS_DIR}/mkmodule")
    message(FATAL_ERROR
      "udynlink_add_module: udynlink_SCRIPTS_DIR is not set or does not point at the "
      "udynlink scripts (expected .../scripts). Consume udynlink via "
      "add_subdirectory / FetchContent / find_package first.")
  endif()

  # Locate the udynlink public headers so module sources can #include them
  # (e.g. udynlink_deps_api.h). In-tree/FetchContent consumers get it from
  # the root CMakeLists cache entry; installed consumers from
  # udynlinkConfig.cmake. Fall back to the sibling of scripts/ for layouts
  # that skipped both.
  if(NOT udynlink_INCLUDE_DIR)
    get_filename_component(_udynlink_inc_dir "${udynlink_SCRIPTS_DIR}/../udynlink" ABSOLUTE)
    set(udynlink_INCLUDE_DIR "${_udynlink_inc_dir}")
  endif()
  if(NOT EXISTS "${udynlink_INCLUDE_DIR}/udynlink_deps_api.h")
    message(FATAL_ERROR
      "udynlink_add_module: udynlink_INCLUDE_DIR does not point at the udynlink "
      "headers (expected udynlink_deps_api.h next to udynlink.h): "
      "${udynlink_INCLUDE_DIR}")
  endif()

  set(_script "${udynlink_SCRIPTS_DIR}/mkmodule")
  # Intermediate artifacts stay in the build tree, never beside sources.
  if(ARG_WORKDIR)
    get_filename_component(_workdir "${ARG_WORKDIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  else()
    set(_workdir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${name}.mkmodule")
  endif()
  file(MAKE_DIRECTORY "${_workdir}")
  set(_bin "${ARG_OUTPUT_DIR}/${name}.bin")
  set(_header "${ARG_HEADER_OUTPUT_DIR}/${name}_module_data.h")

  foreach(_s IN LISTS ARG_SOURCES)
    get_filename_component(_abs "${_s}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND _abs_sources "${_abs}")
  endforeach()

  # Rebuild when the toolchain changes, not only when sources change.
  set(_script_deps
    "${_script}"
    "${udynlink_SCRIPTS_DIR}/udynlink_utils.py"
    "${udynlink_SCRIPTS_DIR}/udynlink_parser.py"
    "${udynlink_SCRIPTS_DIR}/targets.py"
    "${udynlink_SCRIPTS_DIR}/code_before_data.ld"
    "${udynlink_SCRIPTS_DIR}/linker_sections.ld.j2"
    "${udynlink_SCRIPTS_DIR}/cpp_init_fini.c")
  file(GLOB _tmpl_deps "${udynlink_SCRIPTS_DIR}/asm_template*.tmpl")
  list(APPEND _script_deps ${_tmpl_deps})

  set(_args
    "--no-verbose" "--no-debug"
    "--target" "${ARG_TARGET}"
    "--workdir" "${_workdir}"
    "--bin-name" "${_bin}"
    "--module-name" "${ARG_MODULE_NAME}"
    "-I" "${udynlink_INCLUDE_DIR}"
    "-O" "${ARG_OPT_LEVEL}"
    "--mod-version" "${ARG_MOD_VERSION}")
  if(ARG_UDYNLINK_VERSION)
    list(APPEND _args "--udynlink-version" "${ARG_UDYNLINK_VERSION}")
  endif()
  # Caller include dirs come after the udynlink headers, so a module source
  # reaches its own headers first and udynlink's names stay unambiguous.
  foreach(_inc IN LISTS ARG_INCLUDE_DIRS)
    get_filename_component(_abs_inc "${_inc}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    list(APPEND _args "-I" "${_abs_inc}")
  endforeach()
  # Placement sections: each entry is a raw mkmodule spec
  # (NAME[:align=N][:flags=F1,F2]). Declaring one raises the loader ABI to
  # 3.1 automatically (mkmodule), so no UDYNLINK_VERSION is needed here.
  foreach(_sec IN LISTS ARG_SECTIONS)
    list(APPEND _args "--section" "${_sec}")
  endforeach()
  if(ARG_TAG_ON_ALIGN)
    list(APPEND _args "--tag-on-align")
  endif()
  if(ARG_STRIP_HIDDEN_SYMS)
    list(APPEND _args "--strip-hidden-syms")
  endif()
  if(ARG_STRIP_NON_PUBLIC_SYMS)
    list(APPEND _args "--strip-non-public-syms")
  endif()
  if(ARG_STRIP_MANGLED_SYMS)
    list(APPEND _args "--strip-mangled-syms")
  endif()
  if(ARG_STRIP_WEAK_SYM_NAMES)
    list(APPEND _args "--strip-weak-sym-names")
  endif()
  if(ARG_MCPU)
    list(APPEND _args "--mcpu" "${ARG_MCPU}")
  endif()
  if(ARG_PUBLIC_SYMBOLS)
    list(APPEND _args "--public-symbols" "${ARG_PUBLIC_SYMBOLS}")
  endif()
  if(ARG_BUILD_FLAGS)
    list(APPEND _args "--build-flags" "${ARG_BUILD_FLAGS}")
  endif()
  if(ARG_NO_PROLOGUE)
    list(APPEND _args "--no-prologue")
  endif()
  if(ARG_PC_REL)
    list(APPEND _args "--pc-rel")
  endif()
  if(ARG_NO_LONG_CALLS)
    list(APPEND _args "--no-long-calls")
  endif()
  if(ARG_LTO)
    list(APPEND _args "--lto")
  endif()
  if(ARG_DISASM)
    list(APPEND _args "--disasm")
  endif()
  if(ARG_GENERATE_HEADER OR ARG_IMPORTS_HEADER)
    list(APPEND _args "--header-path" "${ARG_HEADER_OUTPUT_DIR}")
  endif()
  if(ARG_GENERATE_HEADER)
    list(APPEND _args "--gen-c-header")
  endif()
  if(ARG_IMPORTS_HEADER)
    list(APPEND _args "--gen-imports-header")
  endif()
  list(APPEND _args ${_abs_sources})

  set(_outputs "${_bin}")
  if(ARG_GENERATE_HEADER)
    list(APPEND _outputs "${_header}")
  endif()

  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
    COMMAND Python3::Interpreter "${_script}" ${_args}
    DEPENDS ${_abs_sources} ${_script_deps} ${ARG_DEPENDS}
    COMMENT "Building udynlink module: ${name}"
    VERBATIM)

  # Builds the module when the project is built, and on bare `cmake --build`.
  add_custom_target(${name} ALL DEPENDS ${_bin})

  # Consumer-facing: link this to pull the module build + the header include dir.
  # A plain-named INTERFACE target carries the build-ordering dependency and
  # the include dir; a namespaced ALIAS exposes it as udynlink::module::<name>
  # (regular targets may not use '::' in their name, but ALIAS targets may).
  #
  # NOTE: the ordering edge is add_dependencies(), NOT putting the module
  # custom target into INTERFACE_LINK_LIBRARIES — modern CMake (4.x) rejects
  # linking UTILITY targets and older versions leak the name as a raw -l flag.
  set(_iface_tgt "_udynlink_module_${name}_iface")
  add_library(${_iface_tgt} INTERFACE)
  add_dependencies(${_iface_tgt} ${name})
  if(ARG_GENERATE_HEADER OR ARG_IMPORTS_HEADER)
    target_include_directories(${_iface_tgt} INTERFACE "${ARG_HEADER_OUTPUT_DIR}")
  endif()
  add_library(udynlink::module::${name} ALIAS ${_iface_tgt})
endfunction()
