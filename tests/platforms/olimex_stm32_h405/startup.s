    .syntax unified
    .arch armv7e-m
    .thumb

    .section .isr_vector, "a"
    .align 2
    .globl __vectors
__vectors:
    .word _estack               /* Initial SP */
    .word Reset_Handler         /* Reset */
    .word NMI_Handler           /* NMI */
    .word HardFault_Handler     /* HardFault */
    .word MemManage_Handler     /* MemManage */
    .word BusFault_Handler      /* BusFault */
    .word UsageFault_Handler    /* UsageFault */
    .word 0, 0, 0, 0            /* Reserved */
    .word SVC_Handler           /* SVCall */
    .word DebugMon_Handler      /* DebugMon */
    .word 0                     /* Reserved */
    .word PendSV_Handler        /* PendSV */
    .word SysTick_Handler       /* SysTick */
    /* External interrupts: default handler for all unused IRQs */
    .rept 100
    .word Default_Handler
    .endr

    .section .text.Reset_Handler
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Enable FPU (CP10 & CP11 full access) */
    ldr  r0, =0xE000ED88
    ldr  r1, [r0]
    orr  r1, r1, #(0xF << 20)
    str  r1, [r0]
    dsb
    isb

    /* Copy .data from Flash to RAM */
    ldr  r0, =_sidata
    ldr  r1, =_sdata
    ldr  r2, =_edata
    b    2f
1:
    ldr  r3, [r0], #4
    str  r3, [r1], #4
2:
    cmp  r1, r2
    blo  1b

    /* Zero .bss */
    ldr  r0, =_sbss
    ldr  r1, =_ebss
    movs r2, #0
    b    4f
3:
    str  r2, [r0], #4
4:
    cmp  r0, r1
    blo  3b

    /* Call main */
    bl   main
    b    .                      /* Hang if main returns */

    .thumb_func
    .weak Default_Handler
Default_Handler:
    b    .

    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b    .

    .thumb_func
    .weak HardFault_Handler
HardFault_Handler:
    b    .

    .thumb_func
    .weak MemManage_Handler
MemManage_Handler:
    b    .

    .thumb_func
    .weak BusFault_Handler
BusFault_Handler:
    b    .

    .thumb_func
    .weak UsageFault_Handler
UsageFault_Handler:
    b    .

    .thumb_func
    .weak SVC_Handler
SVC_Handler:
    b    .

    .thumb_func
    .weak DebugMon_Handler
DebugMon_Handler:
    b    .

    .thumb_func
    .weak PendSV_Handler
PendSV_Handler:
    b    .

    .thumb_func
    .weak SysTick_Handler
SysTick_Handler:
    b    .
