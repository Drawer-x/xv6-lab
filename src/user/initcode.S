    .section .text
    .globl _start

    .equ SYS_print_str, 4

_start:
    # 把字符串地址放进 a0
    la a0, msg

    # syscall: print_str(msg)
    li a7, SYS_print_str
    ecall

1:
    j 1b

    .section .rodata
msg:
    .asciz "hello, world!\n"
