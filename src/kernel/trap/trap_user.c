#include "mod.h"
#include "../lib/method.h"
#include "../mem/method.h"
#include "../mem/type.h"      // 引入 MAKE_SATP
#include "../proc/method.h"
#include "../proc/type.h"
#include "../../user/syscall_num.h"

// 汇编符号
extern char trampoline[];
extern char user_vector[];
extern void user_return(trapframe_t *tf, uint64 satp);

/*
 * 用户态陷阱入口 (由 trampoline.S 的 user_vector 跳转进入)
 * 注意：user_vector 是 “jr t0” 跳到这里的，而不是调用，所以不能 return。
 * 处理完成后必须调用 user_return(tf, MAKE_SATP(p->pgtbl)) 回到用户态。
 */
void trap_user_handler()
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_handler: null trapframe");

    uint64 scause = r_scause();
    uint64 is_intr = (scause >> 63) & 1;
    uint64 code   = scause & 0xfff;

    if (!is_intr) {
        // ---- 异常 ----
        if (code == 8) { // ecall from U (系统调用)
            uint64 num = tf->a7; // 系统调用号存放在 a7
            switch (num) {
                case SYS_helloworld:
                    printf("proczero: hello world\n");
                    break;
                default:
                    printf("user syscall unknown: %lu\n", num);
                    break;
            }
            // 跳过 ecall 指令，防止无限陷入
            tf->user_to_kern_epc = r_sepc() + 4;
        } else {
            // 其他异常，直接报错
            printf("user exception: scause=%lx sepc=%lx stval=%lx\n",
                   scause, r_sepc(), r_stval());
            panic("unhandled user exception");
        }
    } else {
        // ---- 中断 ----（本实验阶段直接忽略返回）
        tf->user_to_kern_epc = r_sepc();
    }

    // 设置返回环境并回到用户态
    w_stvec((uint64)user_vector);
    w_sscratch((uint64)tf);

    // 使用宏 MAKE_SATP 返回到用户态
    user_return(tf, MAKE_SATP(p->pgtbl));
    __builtin_unreachable();
}

/*
 * 首次从内核进入用户态（proczero 的 ctx.ra = trap_user_return）
 * 类似 xv6 的 usertrapret。
 */
void trap_user_return()
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_return: null trapframe");

    // sepc 指向用户代码入口
    w_sepc(tf->user_to_kern_epc);
    // stvec 指向用户态陷阱入口
    w_stvec((uint64)user_vector);
    // sscratch 保存 trapframe 地址
    w_sscratch((uint64)tf);

    // ✅ 切换页表并 sret 回用户态
    user_return(tf, MAKE_SATP(p->pgtbl));
    __builtin_unreachable();
}
