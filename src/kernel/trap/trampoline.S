    .section .trampoline, "ax"
    .balign 16

    .globl trampoline
trampoline:

    .globl user_vector
user_vector:
        # 进入时：
        #   sscratch = trapframe*
        #   a0       = 0 (用户态 a0 的值对我们来说没意义)
        #
        # csrrw a0, sscratch, a0:
        #   a0 <- old sscratch (trapframe*)
        #   sscratch <- old a0 (用户态a0)
        csrrw   a0, sscratch, a0    # a0 = tf, sscratch = user_a0

        # -------- 保存通用寄存器到 trapframe (按 trapframe_t 布局) --------
        sd      ra, 40(a0)
        sd      sp, 48(a0)
        sd      gp, 56(a0)
        sd      tp, 64(a0)

        sd      t0, 72(a0)
        sd      t1, 80(a0)
        sd      t2, 88(a0)

        sd      s0, 96(a0)
        sd      s1, 104(a0)

        # 把“用户态的 a0”保存在 trapframe->saved_sscratch (offset 112)
        csrr    t0, sscratch
        sd      t0, 112(a0)

        sd      a1, 120(a0)
        sd      a2, 128(a0)
        sd      a3, 136(a0)
        sd      a4, 144(a0)
        sd      a5, 152(a0)
        sd      a6, 160(a0)
        sd      a7, 168(a0)

        sd      s2, 176(a0)
        sd      s3, 184(a0)
        sd      s4, 192(a0)
        sd      s5, 200(a0)
        sd      s6, 208(a0)
        sd      s7, 216(a0)
        sd      s8, 224(a0)
        sd      s9, 232(a0)
        sd      s10,240(a0)
        sd      s11,248(a0)

        sd      t3, 256(a0)
        sd      t4, 264(a0)
        sd      t5, 272(a0)
        sd      t6, 280(a0)

        # -------- 切回内核执行上下文 --------
        # 这里我们依赖 proc_make_first() 预填的前5个槽：
        #   0   user_to_kern_satp        (内核页表的 satp)
        #   8   user_to_kern_sp          (进内核用的内核栈顶)
        #   16  user_to_kern_trapvector  (trap_user_handler 函数指针)
        #   24  user_to_kern_epc         (用户下一次 sepc，但这里我们不需要)
        #   32  user_to_kern_hartid      (内核 tp)
        ld      sp, 8(a0)          # switch SP 到内核栈 (注意: 不要写这个槽，只读)
        ld      tp, 32(a0)         # 恢复内核里的 tp (hartid)
        ld      t0, 16(a0)         # t0 = trap_user_handler (C 函数)
        ld      t1, 0(a0)          # t1 = 内核页表的 satp
        csrw    satp, t1           # 切回内核页表
        sfence.vma zero, zero

        jr      t0                 # 跳到 trap_user_handler(tf 在 a0 里)
                                   # 注意: 我们保持 a0=trapframe* 不动
                                   #       C 代码正是用 myproc()->tf 来拿它


# ===========================================================
# user_return: S -> U
#   Called from kernel when we want to resume user mode.
#   a0 = trapframe*
#   a1 = user_satp (MAKE_SATP(p->pgtbl))
#
#   We have to:
#     1. Save that user_satp somewhere safe.
#     2. Restore user regs (but carefully not to clobber that copy too early).
#     3. Switch to user page table.
#     4. sret -> jump to sepc with U-mode.
# ===========================================================
    .globl user_return
user_return:
        # a0 = trapframe*
        # a1 = user_satp (MAKE_SATP(p->pgtbl))

        # (0) stash what we need post-satp
        mv      t0, a1          # t0 := user_satp
        ld      t1, 72(a0)      # t1 := user t0 (trapframe->t0)

        # (1) restore user registers (except t0 and a0 for now)
        ld      ra, 40(a0)
        ld      sp, 48(a0)
        ld      gp, 56(a0)
        ld      tp, 64(a0)

        # skip t0 slot (72) - we'll restore it later
        ld      t2, 80(a0)
        ld      t3, 88(a0)

        ld      s0, 96(a0)
        ld      s1, 104(a0)

        # 恢复 a1-a7
        ld      a1, 120(a0)
        ld      a2, 128(a0)
        ld      a3, 136(a0)
        ld      a4, 144(a0)
        ld      a5, 152(a0)
        ld      a6, 160(a0)
        ld      a7, 168(a0)

        ld      s2, 176(a0)
        ld      s3, 184(a0)
        ld      s4, 192(a0)
        ld      s5, 200(a0)
        ld      s6, 208(a0)
        ld      s7, 216(a0)
        ld      s8, 224(a0)
        ld      s9, 232(a0)
        ld      s10,240(a0)
        ld      s11,248(a0)

        ld      t3, 256(a0)
        ld      t4, 264(a0)
        ld      t5, 272(a0)
        ld      t6, 280(a0)

        # (2) 现在恢复 a0 - 必须放在最后
        ld      a0, 112(a0)     # 从 saved_sscratch 恢复用户态的 a0

        # (3) switch to user page table
        csrw    satp, t0
        sfence.vma zero, zero

        # (4) restore user's t0
        mv      t0, t1

        # (5) return to user
        sret