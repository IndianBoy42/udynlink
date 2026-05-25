    .syntax unified
    .arch armv7-m

    .section .isr_vector, "a"
    .align 2
    .globl __vectors
__vectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler
    /* External interrupts (weak, default to Default_Handler) */
    .word Default_Handler  /* WWDG */
    .word Default_Handler  /* PVD */
    .word Default_Handler  /* TAMPER */
    .word Default_Handler  /* RTC */
    .word Default_Handler  /* FLASH */
    .word Default_Handler  /* RCC */
    .word Default_Handler  /* EXTI0 */
    .word Default_Handler  /* EXTI1 */
    .word Default_Handler  /* EXTI2 */
    .word Default_Handler  /* EXTI3 */
    .word Default_Handler  /* EXTI4 */
    .word Default_Handler  /* DMA1_Channel1 */
    .word Default_Handler  /* DMA1_Channel2 */
    .word Default_Handler  /* DMA1_Channel3 */
    .word Default_Handler  /* DMA1_Channel4 */
    .word Default_Handler  /* DMA1_Channel5 */
    .word Default_Handler  /* DMA1_Channel6 */
    .word Default_Handler  /* DMA1_Channel7 */
    .word Default_Handler  /* ADC1_2 */
    .word Default_Handler  /* CAN1_TX */
    .word Default_Handler  /* CAN1_RX0 */
    .word Default_Handler  /* CAN1_RX1 */
    .word Default_Handler  /* CAN1_SCE */
    .word Default_Handler  /* EXTI9_5 */
    .word Default_Handler  /* TIM1_BRK */
    .word Default_Handler  /* TIM1_UP */
    .word Default_Handler  /* TIM1_TRG_COM */
    .word Default_Handler  /* TIM1_CC */
    .word Default_Handler  /* TIM2 */
    .word Default_Handler  /* TIM3 */
    .word Default_Handler  /* TIM4 */
    .word Default_Handler  /* I2C1_EV */
    .word Default_Handler  /* I2C1_ER */
    .word Default_Handler  /* I2C2_EV */
    .word Default_Handler  /* I2C2_ER */
    .word Default_Handler  /* SPI1 */
    .word Default_Handler  /* SPI2 */
    .word Default_Handler  /* USART1 */
    .word Default_Handler  /* USART2 */
    .word Default_Handler  /* USART3 */
    .word Default_Handler  /* EXTI15_10 */
    .word Default_Handler  /* RTCAlarm */
    .word Default_Handler  /* TIM8_BRK */
    .word Default_Handler  /* TIM8_UP */
    .word Default_Handler  /* TIM8_TRG_COM */
    .word Default_Handler  /* TIM8_CC */
    .word Default_Handler  /* ADC3 */
    .word Default_Handler  /* FSMC */
    .word Default_Handler  /* SDIO */
    .word Default_Handler  /* TIM5 */
    .word Default_Handler  /* SPI3 */
    .word Default_Handler  /* UART4 */
    .word Default_Handler  /* UART5 */
    .word Default_Handler  /* TIM6 */
    .word Default_Handler  /* TIM7 */
    .word Default_Handler  /* DMA2_Channel1 */
    .word Default_Handler  /* DMA2_Channel2 */
    .word Default_Handler  /* DMA2_Channel3 */
    .word Default_Handler  /* DMA2_Channel4_5 */

    .section .text.Reset_Handler
    .thumb
    .thumb_func
    .align 1
    .globl Reset_Handler
    .type Reset_Handler, %function
Reset_Handler:
    /* Copy .data from Flash to RAM */
    ldr     r0, =_sidata
    ldr     r1, =_sdata
    ldr     r2, =_edata
    b       2f
1:
    ldr     r3, [r0], #4
    str     r3, [r1], #4
2:
    cmp     r1, r2
    blo     1b

    /* Zero .bss */
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    movs    r2, #0
    b       4f
3:
    str     r2, [r0], #4
4:
    cmp     r0, r1
    blo     3b

    /* Call SystemInit if defined */
    ldr     r0, =SystemInit
    cmp     r0, #0
    beq     5f
    blx     r0
5:
    /* Call main directly (we already did data/bss init) */
    bl      main
    b       .

    .thumb
    .thumb_func
    .align 1
    .weak Default_Handler
    .type Default_Handler, %function
Default_Handler:
    b       Default_Handler

    .thumb
    .thumb_func
    .align 1
    .weak NMI_Handler
    .thumb_func
    .weak HardFault_Handler
    .thumb_func
    .weak MemManage_Handler
    .thumb_func
    .weak BusFault_Handler
    .thumb_func
    .weak UsageFault_Handler
    .thumb_func
    .weak SVC_Handler
    .thumb_func
    .weak DebugMon_Handler
    .thumb_func
    .weak PendSV_Handler
    .thumb_func
    .weak SysTick_Handler
NMI_Handler:
HardFault_Handler:
MemManage_Handler:
BusFault_Handler:
UsageFault_Handler:
SVC_Handler:
DebugMon_Handler:
PendSV_Handler:
SysTick_Handler:
    b       .

    /* Provide weak SystemInit */
    .weak SystemInit
    .thumb_set SystemInit, 0

    .end
