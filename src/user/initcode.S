    .section .text
    .globl _start

    .equ SYS_helloworld, 1

_start:
    li a7, SYS_helloworld
    ecall

    li a7, SYS_helloworld
    ecall

1:
    j 1b
