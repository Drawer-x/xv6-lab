# 用户内核切换的跳板代码 
# 与 kernel.ld 的 .trampoline 段匹配（单独1页、可执行）
# 重要：一定要带 "ax" 属性，确保可装载且可执行
.section .trampoline, "ax"
.balign 16

# trampoline 会作为一个物理页的起点地址（kernel.ld 已对齐到 4KB）
.globl trampoline
trampoline:

# 用户态 trap 入口：保存用户寄存器到 trapframe，然后切回内核
.align 4
.globl user_vector
user_vector:
        # a0 此时是 trapframe 指针（在进入前由 sscratch 约定），与 sscratch 交换
        csrrw a0, sscratch, a0

        # 保存通用寄存器到 trapframe（偏移需与 C 中 trapframe_t 一致）
        sd ra, 40(a0)
        sd sp, 48(a0)
        sd gp, 56(a0)
        sd tp, 64(a0)
        sd t0, 72(a0)
        sd t1, 80(a0)
        sd t2, 88(a0)
        sd s0, 96(a0)
        sd s1, 104(a0)
        # 注意：这里未保存 a0（此时 a0=trapframe 指针）；若你的 trapframe 需要保存用户态 a0，请在进入前或此处按布局补上
        sd a1, 120(a0)
        sd a2, 128(a0)
        sd a3, 136(a0)
        sd a4, 144(a0)
        sd a5, 152(a0)
        sd a6, 160(a0)
        sd a7, 168(a0)
        sd s2, 176(a0)
        sd s3, 184(a0)
        sd s4, 192(a0)
        sd s5, 200(a0)
        sd s6, 208(a0)
        sd s7, 216(a0)
        sd s8, 224(a0)
        sd s9, 232(a0)
        sd s10, 240(a0)
        sd s11, 248(a0)
        sd t3, 256(a0)
        sd t4, 264(a0)
        sd t5, 272(a0)
        sd t6, 280(a0)

        # 把旧的 sscratch（通常放的是内核 sp/tf 指针等）暂存到 tf 对应槽位
        csrr t0, sscratch
        sd   t0, 112(a0)

        # 恢复内核环境并跳回内核（t0=trap内核入口，t1=satp）
        ld sp, 8(a0)
        ld tp, 32(a0)
        ld t0, 16(a0)
        ld t1, 0(a0)
        csrw satp, t1
        sfence.vma zero, zero
        jr t0

# 用户态 trap 处理完成后返回用户
.globl user_return
user_return:
        csrw satp, a1
        sfence.vma zero, zero

        ld t0, 112(a0)
        csrw sscratch, t0 

        ld ra, 40(a0)
        ld sp, 48(a0)
        ld gp, 56(a0)
        ld tp, 64(a0)
        ld t0, 72(a0)
        ld t1, 80(a0)
        ld t2, 88(a0)
        ld s0, 96(a0)
        ld s1, 104(a0)
        # 若 trapframe_t 布局里保存了用户 a0，请在这里对应恢复
        ld a1, 120(a0)
        ld a2, 128(a0)
        ld a3, 136(a0)
        ld a4, 144(a0)
        ld a5, 152(a0)
        ld a6, 160(a0)
        ld a7, 168(a0)
        ld s2, 176(a0)
        ld s3, 184(a0)
        ld s4, 192(a0)
        ld s5, 200(a0)
        ld s6, 208(a0)
        ld s7, 216(a0)
        ld s8, 224(a0)
        ld s9, 232(a0)
        ld s10, 240(a0)
        ld s11, 248(a0)
        ld t3, 256(a0)
        ld t4, 264(a0)
        ld t5, 272(a0)
        ld t6, 280(a0)

        csrrw a0, sscratch, a0
        sret
