# tests/platforms/stm32f103_bluepill/platform.cmake
# Platform-specific settings for STM32F103 (Blue Pill / NUCLEO-F103RB, Cortex-M3)

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
    -fno-move-loop-invariants
    -Wall
    -Wextra
    -g3
    -Wno-format
)

# Common definitions
set(UDYNLINK_PLATFORM_DEFINES
    DEBUG
    USE_FULL_ASSERT
    OS_USE_SEMIHOSTING
    TRACE
    OS_USE_TRACE_SEMIHOSTING_DEBUG
    STM32F10X_MD
    HSE_VALUE=8000000
    UDYNLINK_HOST_ARCH_TAG=UDYNLINK_ARCH_TAG_CORTEX_M3
)

# Include paths (relative to tests/qemu_host/)
set(UDYNLINK_PLATFORM_INCLUDE_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
)

# Linker flags
set(UDYNLINK_PLATFORM_LINK_OPTIONS
    -Wl,-T,mem.ld
    -Wl,-T,libs.ld
    -Wl,-T,sections.ld
    -nostartfiles
    -Wl,--gc-sections
    -Wl,-Map,test1.map
    --specs=nano.specs
)

# Linker script search path
set(UDYNLINK_PLATFORM_LINK_DIRS
    ${CMAKE_CURRENT_LIST_DIR}
)

# Platform-specific sources (self-contained, no HAL)
set(UDYNLINK_PLATFORM_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/startup.s
    ${CMAKE_CURRENT_LIST_DIR}/system_init.c
    ${CMAKE_CURRENT_LIST_DIR}/syscalls.c
    ${CMAKE_CURRENT_LIST_DIR}/my_printf.c
)
