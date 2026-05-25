# cmake/platforms/stm32f429_discovery.cmake
# Platform-specific settings for STM32F429I-Discovery (Cortex-M4)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER arm-none-eabi-gcc CACHE STRING "C compiler" FORCE)
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER arm-none-eabi-g++ CACHE STRING "C++ compiler" FORCE)
endif()

# Target CPU flags
set(UDYNLINK_PLATFORM_CFLAGS
    -mcpu=cortex-m4
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
    STM32F429xx
    USE_HAL_DRIVER
    HSE_VALUE=8000000
    UDYNLINK_LOT_BASE_ADDR=0x20000000
)

# Include paths (relative to tests/qemu_host/)
set(UDYNLINK_PLATFORM_INCLUDE_DIRS
    include
    system/include
    system/include/cmsis
    system/include/stm32f4-hal
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
    ldscripts
)
