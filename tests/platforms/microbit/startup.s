    .syntax unified
    .arch armv6-m
    .thumb

    .section .isr_vector, "a"
    .align 2
    .globl __vectors
__vectors:
    .word _estack               /* Initial SP */
    .word Reset_Handler         /* Reset */
    .word NMI_Handler           /* NMI */
    .word HardFault_Handler     /* HardFault */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word SVC_Handler           /* SVCall */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word PendSV_Handler        /* PendSV */
    .word SysTick_Handler       /* SysTick */

    .section .text.Reset_Handler
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Copy .data from Flash to RAM */
    ldr  r0, =_sdata
    ldr  r1, =_edata
    ldr  r2, =_sidata
    b    4f
3:
    ldr  r3, [r2]
    str  r3, [r0]
    adds r0, #4
    adds r2, #4
4:
    cmp  r0, r1
    bne  3b

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
    bne  1b

    /* Call main */
    bl   main
    b    .                      /* Hang if main returns */

    .thumb_func
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
    b    .
