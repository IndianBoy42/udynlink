    .syntax unified
    .arch armv7-m
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
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word SVC_Handler           /* SVCall */
    .word DebugMon_Handler      /* DebugMon */
    .word 0                     /* Reserved */
    .word PendSV_Handler        /* PendSV */
    .word SysTick_Handler       /* SysTick */

    .section .text.Reset_Handler
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Zero .bss */
    ldr  r0, =_sbss
    ldr  r1, =_ebss
    movs r2, #0
    b    2f
1:
    str  r2, [r0]
    adds r0, #4
2:
    cmp  r0, r1
    blo  1b

    /* Call main */
    bl   main
    b    .                      /* Hang if main returns */

    .thumb_func
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
    b    .
