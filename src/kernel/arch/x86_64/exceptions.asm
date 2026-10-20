BITS 64

section .text

extern FeatherOS_x86_64_ExceptionDispatch
extern FeatherOS_x86_64_GetUserReturnStack
extern FeatherOS_x86_64_SetKernelStack
extern FeatherOS_x86_64_SmapEnabled
global x86_64_load_gdt
global x86_64_enable_sse
global x86_64_enter_user
global x86_64_user_test_return
global x86_64_syscall_stub
global x86_64_exception_stub_table
global x86_64_irq_stub_table
global x86_64_spurious_interrupt

x86_64_enable_sse:
    mov rax, cr0
    and eax, 0xfffffff3
    or eax, 0x22
    mov cr0, rax
    mov rax, cr4
    or rax, 0x600
    mov cr4, rax
    fninit
    ldmxcsr [rel default_mxcsr]
    ret

x86_64_load_gdt:
    lgdt [rdi]
    push qword 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    ; GS is not reloaded: that would clear its base, which holds the per-CPU state pointer.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax
    mov ax, 0x28
    ltr ax
    ret

x86_64_enter_user:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    push rdi
    push rsi
    push rdx
    push rcx
    mov [rcx + 8], rsp
    mov rdi, rsp
    sub rsp, 8
    call FeatherOS_x86_64_SetKernelStack
    add rsp, 8
    mov rax, [rsp + 24]
    mov rcx, [rsp + 16]
    mov rdi, [rsp + 8]
    push qword 0x1b
    push rcx
    pushfq
    push qword 0x23
    push rax
    swapgs
    iretq

x86_64_user_test_return:
    mov rsp, rax
    add rsp, 32
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

x86_64_syscall_stub:
    push qword 0
    push qword 128
    jmp x86_64_exception_common

%macro ISR_NO_ERROR 1
global x86_64_exception_%1
x86_64_exception_%1:
    push qword 0
    push qword %1
    jmp x86_64_exception_common
%endmacro

%macro ISR_ERROR 1
global x86_64_exception_%1
x86_64_exception_%1:
    push qword %1
    jmp x86_64_exception_common
%endmacro

ISR_NO_ERROR 0
ISR_NO_ERROR 1
ISR_NO_ERROR 2
ISR_NO_ERROR 3
ISR_NO_ERROR 4
ISR_NO_ERROR 5
ISR_NO_ERROR 6
ISR_NO_ERROR 7
ISR_ERROR 8
ISR_NO_ERROR 9
ISR_ERROR 10
ISR_ERROR 11
ISR_ERROR 12
ISR_ERROR 13
ISR_ERROR 14
ISR_NO_ERROR 15
ISR_NO_ERROR 16
ISR_ERROR 17
ISR_NO_ERROR 18
ISR_NO_ERROR 19
ISR_NO_ERROR 20
ISR_ERROR 21
ISR_NO_ERROR 22
ISR_NO_ERROR 23
ISR_NO_ERROR 24
ISR_NO_ERROR 25
ISR_NO_ERROR 26
ISR_NO_ERROR 27
ISR_NO_ERROR 28
ISR_ERROR 29
ISR_ERROR 30
ISR_NO_ERROR 31

%assign irq_vector 32
%rep 18
global x86_64_irq_%+irq_vector
x86_64_irq_%+irq_vector:
    push qword 0
    push qword irq_vector
    jmp x86_64_exception_common
%assign irq_vector irq_vector + 1
%endrep

x86_64_spurious_interrupt:
    push qword 0
    push qword 255
    jmp x86_64_exception_common

x86_64_exception_common:
    ; Only entries from ring 3 swap GS; the saved CS is above the vector and error code.
    test qword [rsp + 24], 3
    jz .kernel_entry
    swapgs
.kernel_entry:
    ; Close any open user-access window so handlers never inherit one; CLAC needs SMAP.
    cmp byte [rel FeatherOS_x86_64_SmapEnabled], 0
    je .access_closed
    clac
.access_closed:
    cld
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, rsp
    mov r12, rsp
    and rsp, -16
    call FeatherOS_x86_64_ExceptionDispatch
    call FeatherOS_x86_64_GetUserReturnStack
    test rax, rax
    jnz x86_64_user_test_return
    mov rsp, r12
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    test qword [rsp + 8], 3
    jz .kernel_return
    swapgs
.kernel_return:
    iretq

section .rodata
align 8
align 4
default_mxcsr:
    dd 0x1F80

x86_64_exception_stub_table:
    dq x86_64_exception_0, x86_64_exception_1, x86_64_exception_2, x86_64_exception_3
    dq x86_64_exception_4, x86_64_exception_5, x86_64_exception_6, x86_64_exception_7
    dq x86_64_exception_8, x86_64_exception_9, x86_64_exception_10, x86_64_exception_11
    dq x86_64_exception_12, x86_64_exception_13, x86_64_exception_14, x86_64_exception_15
    dq x86_64_exception_16, x86_64_exception_17, x86_64_exception_18, x86_64_exception_19
    dq x86_64_exception_20, x86_64_exception_21, x86_64_exception_22, x86_64_exception_23
    dq x86_64_exception_24, x86_64_exception_25, x86_64_exception_26, x86_64_exception_27
    dq x86_64_exception_28, x86_64_exception_29, x86_64_exception_30, x86_64_exception_31

align 8
x86_64_irq_stub_table:
    dq x86_64_irq_32, x86_64_irq_33, x86_64_irq_34, x86_64_irq_35
    dq x86_64_irq_36, x86_64_irq_37, x86_64_irq_38, x86_64_irq_39
    dq x86_64_irq_40, x86_64_irq_41, x86_64_irq_42, x86_64_irq_43
    dq x86_64_irq_44, x86_64_irq_45, x86_64_irq_46, x86_64_irq_47
    dq x86_64_irq_48, x86_64_irq_49
