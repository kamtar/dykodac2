.syntax unified
.cpu cortex-m7
.thumb

.section .boot_vector,"a",%progbits
.align 10
.global __Vectors
__Vectors:
    .word _vStackTop
    .word boot_flash_entry

.section .boot_trampoline,"ax",%progbits
.align 2
.global boot_flash_entry
.type boot_flash_entry,%function
.thumb_func
boot_flash_entry:
    ldr sp, =_vStackTop
    ldr r0, =__ram_load__
    ldr r1, =__ram_start__
    ldr r2, =__ram_end__
1:  cmp r1, r2
    bcs 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
2:  ldr r1, =__bss_start__
    ldr r2, =__bss_end__
    movs r3, #0
3:  cmp r1, r2
    bcs 4f
    str r3, [r1], #4
    b 3b
4:  ldr r0, =__ram_vectors
    ldr r1, =0xE000ED08
    str r0, [r1]
    dsb
    isb
    bl boot_reset
5:  wfi
    b 5b
.size boot_flash_entry, .-boot_flash_entry

.section .ram_vectors,"a",%progbits
.align 10
.global __ram_vectors
__ram_vectors:
    .word _vStackTop
    .word boot_reset
    .word boot_default_handler
    .word boot_default_handler
    .word boot_default_handler
    .word boot_default_handler
    .word boot_default_handler
    .word 0,0,0,0
    .word boot_default_handler
    .word boot_default_handler
    .word 0
    .word boot_default_handler
    .word boot_default_handler
    .rept 25
    .word boot_default_handler
    .endr
    .word USB_OTG1_IRQHandler
    .rept 102
    .word boot_default_handler
    .endr
