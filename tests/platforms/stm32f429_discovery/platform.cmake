# tests/platforms/stm32f429_discovery/platform.cmake
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
    ${CMAKE_CURRENT_LIST_DIR}
)

# Platform-specific sources
set(UDYNLINK_PLATFORM_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/src/_initialize_hardware.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/src/_write.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/src/stm32f4xx_hal_msp.c

    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/cmsis/system_stm32f4xx.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/cmsis/vectors_stm32f429xx.c

    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/cortexm/_initialize_hardware.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/cortexm/_reset_hardware.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/cortexm/exception_handlers.c

    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/diag/Trace.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/diag/trace_impl.c

    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/_exit.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/_sbrk.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/_startup.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/_syscalls.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/assert.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/_cxx.cpp

    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_iwdg.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c
)

set(UDYNLINK_PLATFORM_HAS_CXX TRUE)

set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/src/stm32f4xx_hal_msp.c
    PROPERTIES COMPILE_OPTIONS "-Wno-missing-prototypes;-Wno-missing-declarations"
)
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/_startup.c
    PROPERTIES COMPILE_DEFINITIONS OS_INCLUDE_STARTUP_INIT_MULTIPLE_RAM_SECTIONS
)
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_cortex.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_flash.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_gpio.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_iwdg.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_pwr.c
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/stm32f4-hal/stm32f4xx_hal_rcc.c
    PROPERTIES COMPILE_OPTIONS "-Wno-bad-function-cast;-Wno-conversion;-Wno-sign-conversion;-Wno-unused-parameter;-Wno-sign-compare;-Wno-missing-prototypes;-Wno-missing-declarations"
)
set_source_files_properties(
    ${CMAKE_CURRENT_LIST_DIR}/../../qemu_host/system/src/newlib/_cxx.cpp
    PROPERTIES
        COMPILE_OPTIONS "-std=gnu++11;-fabi-version=0;-fno-exceptions;-fno-rtti;-fno-use-cxa-atexit;-fno-threadsafe-statics"
        LANGUAGE CXX
)
