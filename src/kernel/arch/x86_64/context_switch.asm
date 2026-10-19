BITS 64

section .text
global FeatherOS_Arch_SwitchContext

; Interrupts stay masked; the switched-in task restores them after releasing the scheduler lock.
FeatherOS_Arch_SwitchContext:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret