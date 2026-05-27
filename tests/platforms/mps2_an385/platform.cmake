# tests/platforms/mps2_an385/platform.cmake
# Platform-specific settings for ARM MPS2-AN385 (Cortex-M3)
# Self-contained: no HAL, no CMSIS, minimal semihosting.

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER arm-none-eabi-gcc CACHE STRING "C compiler" FORCE)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER arm-none-eabi-g++ CACHE STRING "C++ compiler" FORCE)
endif()

# Target CPU flags
set(UDYNLINK_PLATFORM_CFLAGS
    -mcpu=cortex-m3
    -mthumb
    -mfloat-abi=soft
)

# Common compile options
set(UDYNLINK_PLATFORM_COMPILE_OPTIONS
    -Og
    -fmessage-length=0
    -fsigned-char
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
    -g3
)

# Common definitions
set(UDYNLINK_PLATFORM_DEFINES
    UDYNLINK_HOST_ARCH_TAG=UDYNLINK_ARCH_TAG_CORTEX_M3
)

# Include paths
set(UDYNLINK_PLATFORM_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
)

# Linker flags
set(UDYNLINK_PLATFORM_LINK_OPTIONS
    -Wl,-T,mem.ld
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map,test1.map
    --specs=nano.specs
)

# Linker script search path
set(UDYNLINK_PLATFORM_LINK_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
)

# Platform-specific sources
set(UDYNLINK_PLATFORM_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/startup.s
    ${CMAKE_CURRENT_LIST_DIR}/semihosting.c
)

set(UDYNLINK_PLATFORM_HAS_CXX FALSE)
