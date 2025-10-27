    .section .text
    .align 4

    # -------------------------------------------------
    # S-mode trap entry for the kernel
    # -------------------------------------------------
    .globl kernel_vector
kernel_vector:
        addi sp, sp, -256

        sd ra,   0(sp)
        sd sp,   8(sp)
        sd gp,  16(sp)
        sd tp,  24(sp)
        sd t0,  32(sp)
        sd t1,  40(sp)
        sd t2,  48(sp)
        sd s0,  56(sp)
        sd s1,  64(sp)
        sd a0,  72(sp)
        sd a1,  80(sp)
        sd a2,  88(sp)
        sd a3,  96(sp)
        sd a4, 104(sp)
        sd a5, 112(sp)
        sd a6, 120(sp)
        sd a7, 128(sp)
        sd s2, 136(sp)
        sd s3, 144(sp)
        sd s4, 152(sp)
        sd s5, 160(sp)
        sd s6, 168(sp)
        sd s7, 176(sp)
        sd s8, 184(sp)
        sd s9, 192(sp)
        sd s10,200(sp)
        sd s11,208(sp)
        sd t3, 216(sp)
        sd t4, 224(sp)
        sd t5, 232(sp)
        sd t6, 240(sp)

        call trap_kernel_handler

        ld ra,   0(sp)
        ld sp,   8(sp)
        ld gp,  16(sp)
        # we do NOT restore tp from here because tp is hartid in S-mode
        ld t0,  32(sp)
        ld t1,  40(sp)
        ld t2,  48(sp)
        ld s0,  56(sp)
        ld s1,  64(sp)
        ld a0,  72(sp)
        ld a1,  80(sp)
        ld a2,  88(sp)
        ld a3,  96(sp)
        ld a4, 104(sp)
        ld a5, 112(sp)
        ld a6, 120(sp)
        ld a7, 128(sp)
        ld s2, 136(sp)
        ld s3, 144(sp)
        ld s4, 152(sp)
        ld s5, 160(sp)
        ld s6, 168(sp)
        ld s7, 176(sp)
        ld s8, 184(sp)
        ld s9, 192(sp)
        ld s10,200(sp)
        ld s11,208(sp)
        ld t3, 216(sp)
        ld t4, 224(sp)
        ld t5, 232(sp)
        ld t6, 240(sp)

        addi sp, sp, 256

        sret


    # -------------------------------------------------
    # M-mode timer interrupt vector
    # This runs in M-mode when MTIP fires.
    # It arms the next tick and raises SSIP for S-mode.
    # -------------------------------------------------
    .globl timer_vector
    .align 4
timer_vector:
        # --- 调试探针：我们一旦进来，就往 UART MMIO 写 'T' ---
        li      t0, 0x10000000        # UART_BASE
        li      t1, 'T'
        sb      t1, 0(t0)

        # 交换 a0 和 mscratch
        csrrw   a0, mscratch, a0

        # 保存 a1/a2/a3 到 per-hart scratch
        sd      a1, 0(a0)         # [0] = a1
        sd      a2, 8(a0)         # [1] = a2
        sd      a3, 16(a0)        # [2] = a3

        # 取出 mtimecmp ptr 和 interval
        ld      a1, 24(a0)        # a1 = cur[3] = mtimecmp*
        ld      a2, 32(a0)        # a2 = cur[4] = interval
        ld      a3, 0(a1)         # a3 = *mtimecmp
        add     a3, a3, a2        # a3 += interval
        sd      a3, 0(a1)         # *mtimecmp = new compare

        # 向 S-mode 发软件中断：置 sip.SSIP = 1 (bit1)
        li      a1, 2
        csrw    sip, a1

        # 恢复 a1/a2/a3
        ld      a3, 16(a0)
        ld      a2, 8(a0)
        ld      a1, 0(a0)

        # 把 a0 放回 mscratch
        csrrw   a0, mscratch, a0

        mret

