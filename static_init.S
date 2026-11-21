.section .rodata
msg1:
    .ascii "HYC3_STATIC_INIT_START stdout\n"
msg1_end:
msg2:
    .ascii "HYC3_STATIC_INIT_START stderr\n"
msg2_end:

.section .text
.global _start
_start:
    mov $1, %rax
    mov $1, %rdi
    lea msg1(%rip), %rsi
    mov $(msg1_end - msg1), %rdx
    syscall

    mov $1, %rax
    mov $2, %rdi
    lea msg2(%rip), %rsi
    mov $(msg2_end - msg2), %rdx
    syscall

1:
    mov $34, %rax
    syscall
    jmp 1b
