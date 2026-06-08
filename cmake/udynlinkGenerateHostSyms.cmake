#[=======================================================================[.rst:
udynlink_generate_host_syms
----------------------------

Generate a GNU hash table header from a host firmware ELF for O(1) symbol
resolution at module load time.

.. command:: udynlink_generate_host_syms

  .. code-block:: cmake

    udynlink_generate_host_syms(
      ELF <target_or_path>     # Executable target or path to host ELF
      OUTPUT <header_path>     # Output C header path
      [FILTER <regex>]         # Only include symbols matching regex
      [DEPENDS <deps...>]      # Additional dependencies
    )

This is designed for the **offline generation** workflow: run mkhostsyms
against your firmware ELF, commit the generated header, and add a CI
staleness check to detect when it needs regeneration.

Example usage with FetchContent:

.. code-block:: cmake

  include(FetchContent)
  FetchContent_Declare(udynlink GIT_REPOSITORY ... GIT_TAG ...)
  FetchContent_MakeAvailable(udynlink)

  add_executable(firmware main.c driver.c)
  target_link_libraries(firmware PRIVATE udynlink::udynlink)

  # Generate host_syms.h as a post-build step
  udynlink_generate_host_syms(
    ELF firmware
    OUTPUT ${CMAKE_SOURCE_DIR}/src/host_syms.h
  )
#]=======================================================================]

function(udynlink_generate_host_syms)
  cmake_parse_arguments(ARG "" "ELF;OUTPUT;FILTER" "DEPENDS" ${ARGN})

  if(NOT ARG_ELF)
    message(FATAL_ERROR "udynlink_generate_host_syms: ELF is required")
  endif()
  if(NOT ARG_OUTPUT)
    message(FATAL_ERROR "udynlink_generate_host_syms: OUTPUT is required")
  endif()

  find_package(Python3 REQUIRED COMPONENTS Interpreter)

  if(TARGET ${ARG_ELF})
    set(_elf_file "$<TARGET_FILE:${ARG_ELF}>")
    set(_elf_dep "${ARG_ELF}")
  else()
    set(_elf_file "${ARG_ELF}")
    set(_elf_dep "")
  endif()

  if(ARG_FILTER)
    set(_filter_arg "--filter" "${ARG_FILTER}")
  else()
    set(_filter_arg)
  endif()

  set(_script "${udynlink_SCRIPTS_DIR}/mkhostsyms")

  add_custom_command(
    OUTPUT "${ARG_OUTPUT}"
    COMMAND Python3::Interpreter "${_script}"
      --elf "${_elf_file}"
      --output "${ARG_OUTPUT}"
      ${_filter_arg}
    DEPENDS ${_elf_dep} "${_script}" ${ARG_DEPENDS}
    COMMENT "Generating host symbol hash table: ${ARG_OUTPUT}"
  )
endfunction()
