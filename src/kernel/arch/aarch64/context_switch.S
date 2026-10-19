.section .text
.global FeatherOS_Arch_SwitchContext
.type FeatherOS_Arch_SwitchContext, %function

// Interrupts stay masked; the switched-in task restores them after releasing the scheduler lock.

FeatherOS_Arch_SwitchContext:
    stp x29, x30, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    // SP_EL0 is the task's user stack pointer. Before exception setup moves the kernel to SPSel=1,
    // SP_EL0 is the current stack and cannot be named, so skip it then.
    mov x9, xzr
    mrs x10, SPSel
    cbz x10, 1f
    mrs x9, SP_EL0
1:
    stp x9, xzr, [sp, #-16]!
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x9, x10, [sp], #16
    mrs x10, SPSel
    cbz x10, 2f
    msr SP_EL0, x9
2:
    ldp x19, x20, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x29, x30, [sp], #16
    ret