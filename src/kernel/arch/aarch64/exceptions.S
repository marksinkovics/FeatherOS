.section .text
.balign 2048
.global aarch64_exception_vectors
.global aarch64_enter_user
.global aarch64_user_test_return
.type aarch64_exception_vectors, %function

aarch64_exception_vectors:
    b aarch64_exception_common
    .space 124
    b aarch64_irq_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_irq_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_irq_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_irq_common
    .space 124
    b aarch64_exception_common
    .space 124
    b aarch64_exception_common
    .space 124

aarch64_enter_user:
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x29, x30, [sp, #-16]!
    mov x10, sp
    str x10, [x3, #8]
    msr SPSel, #0
    mov sp, x1
    msr SPSel, #1
    mov x9, #0x3c0
    ldrb w10, [x3, #34]
    cbz w10, .Luser_irqs_masked
    sub x9, x9, #0x80
.Luser_irqs_masked:
    msr SPSR_EL1, x9
    msr ELR_EL1, x0
    mov x0, x2
    eret

aarch64_user_test_return:
    ldr x9, [x0, #8]
    mov sp, x9
    ldp x29, x30, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ret

.type aarch64_exception_common, %function
aarch64_exception_common:
    sub sp, sp, #288
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x0, ESR_EL1
    str x0, [sp, #248]
    mrs x0, FAR_EL1
    str x0, [sp, #256]
    mrs x0, ELR_EL1
    str x0, [sp, #264]
    mrs x0, SPSR_EL1
    str x0, [sp, #272]
    mov x0, sp
    bl FeatherOS_AArch64_ExceptionDispatch
    ldr x0, [sp, #264]
    msr ELR_EL1, x0
    ldr x0, [sp, #272]
    msr SPSR_EL1, x0
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #288
    eret

.type aarch64_irq_common, %function
aarch64_irq_common:
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x0, ELR_EL1
    str x0, [sp, #248]
    mrs x0, SPSR_EL1
    str x0, [sp, #256]
    mov x0, sp
    bl FeatherOS_AArch64_IrqDispatch
    mov x0, sp
    bl FeatherOS_AArch64_InterruptReturn
    ldr x0, [sp, #248]
    msr ELR_EL1, x0
    ldr x0, [sp, #256]
    msr SPSR_EL1, x0
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #272
    eret