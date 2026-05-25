    .syntax unified
    .arch armv6-m

    .section .isr_vector, "a"
    .align 2
    .globl __vectors
__vectors:
    .word _estack
    .word Reset_Handler
    .word NMI_Handler
    .word HardFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word 0
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler
    /* External interrupts (weak, default to Default_Handler) */
    .word Default_Handler  /* WWDG */
    .word Default_Handler  /* PVD */
    .word Default_Handler  /* RTC */
    .word Default_Handler  /* FLASH */
    .word Default_Handler  /* RCC */
    .word Default_Handler  /* EXTI0_1 */
    .word Default_Handler  /* EXTI2_3 */
    .word Default_Handler  /* EXTI4_15 */
    .word Default_Handler  /* TSC */
    .word Default_Handler  /* DMA1_Channel1 */
    .word Default_Handler  /* DMA1_Channel2_3 */
    .word Default_Handler  /* DMA1_Channel4_5 */
    .word Default_Handler  /* ADC1 */
    .word Default_Handler  /* TIM1_BRK_UP_TRG_COM */
    .word Default_Handler  /* TIM1_CC */
    .word Default_Handler  /* TIM2 */
    .word Default_Handler  /* TIM3 */
    .word Default_Handler  /* TIM6_DAC */
    .word Default_Handler  /* TIM7 */
    .word Default_Handler  /* TIM14 */
    .word Default_Handler  /* TIM15 */
    .word Default_Handler  /* TIM16 */
    .word Default_Handler  /* TIM17 */
    .word Default_Handler  /* I2C1 */
    .word Default_Handler  /* I2C2 */
    .word Default_Handler  /* SPI1 */
    .word Default_Handler  /* SPI2 */
    .word Default_Handler  /* USART1 */
    .word Default_Handler  /* USART2 */
    .word Default_Handler  /* USART3_6 */
    .word Default_Handler  /* CEC */
    .word Default_Handler  /* CAN */

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
    ldr     r3, [r0]
    str     r3, [r1]
    adds    r0, r0, #4
    adds    r1, r1, #4
2:
    cmp     r1, r2
    blo     1b

    /* Zero .bss */
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    movs    r2, #0
    b       4f
3:
    str     r2, [r0]
    adds    r0, r0, #4
4:
    cmp     r0, r1
    blo     3b

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
    .weak SVC_Handler
    .thumb_func
    .weak PendSV_Handler
    .thumb_func
    .weak SysTick_Handler
NMI_Handler:
HardFault_Handler:
SVC_Handler:
PendSV_Handler:
SysTick_Handler:
    b       .

    .end
