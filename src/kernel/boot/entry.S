    .section .text
    .globl _entry
_entry:
    la      sp, CPU_stack
    li      a0, 4096
    csrr    a1, mhartid
    addi    a1, a1, 1
    mul     a0, a0, a1
    add     sp, sp, a0
    call    start
1:  j 1b
