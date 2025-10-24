    .section .text
    .globl _entry
_entry:
    # 初始化每个 hart 的栈指针
    la      sp, CPU_stack
    li      a0, 4096
    csrr    a1, mhartid
    addi    a1, a1, 1
    mul     a0, a0, a1
    add     sp, sp, a0

    # ===== 新增：M -> S 委派设置 =====
    # 委派用户态异常：U-ECALL(8)、Load/PageFault(13,15)
    li      t0, (1 << 8) | (1 << 12) | (1 << 13) | (1 << 15)
    csrs    medeleg, t0

    # 委派中断：S 软件/时钟/外部中断 (bits 1,5,9)
    li      t0, (1 << 1) | (1 << 5) | (1 << 9)
    csrs    mideleg, t0
    # =================================

    call    start
1:  j 1b
