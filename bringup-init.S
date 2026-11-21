.section .text
.global _start
_start:
    mov $257, %rax
    mov $-100, %rdi
    lea ttyS0_path(%rip), %rsi
    mov $0x901, %rdx
    xor %r10, %r10
    syscall
    mov %rax, %r13

    mov $257, %rax
    mov $-100, %rdi
    lea ttyS1_path(%rip), %rsi
    mov $0x901, %rdx
    xor %r10, %r10
    syscall
    mov %rax, %r14

    mov $257, %rax
    mov $-100, %rdi
    lea kmsg_path(%rip), %rsi
    mov $0x901, %rdx
    xor %r10, %r10
    syscall
    mov %rax, %r15

    lea msg_started(%rip), %rsi
    mov $msg_started_len, %rdx
    call write_msg

    lea msg_loop(%rip), %rsi
    mov $msg_loop_len, %rdx
    call write_msg

1:
    lea msg_alive(%rip), %rsi
    mov $msg_alive_len, %rdx
    call write_msg

    mov $35, %rax
    lea ts(%rip), %rdi
    xor %rsi, %rsi
    syscall
    jmp 1b

write_msg:
    mov $1, %rax
    mov $1, %rdi
    syscall

    mov $1, %rax
    mov $2, %rdi
    syscall

    test %r13, %r13
    js 2f
    mov $1, %rax
    mov %r13, %rdi
    syscall
2:
    test %r14, %r14
    js 3f
    mov $1, %rax
    mov %r14, %rdi
    syscall
3:
    test %r15, %r15
    js 4f
    mov $1, %rax
    mov %r15, %rdi
    syscall
4:
    ret

.section .rodata
ttyS0_path:
    .asciz "/dev/ttyS0"
ttyS1_path:
    .asciz "/dev/ttyS1"
kmsg_path:
    .asciz "/dev/kmsg"
msg_started:
    .ascii "HYC3-BRINGUP: init started\n"
.set msg_started_len, . - msg_started
msg_loop:
    .ascii "HYC3-BRINGUP: entering idle loop\n"
.set msg_loop_len, . - msg_loop
msg_alive:
    .ascii "HYC3-BRINGUP: alive\n"
.set msg_alive_len, . - msg_alive

.section .data
.align 8
ts:
    .quad 30
    .quad 0
