#include "mod.h"
#include "../lib/method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../proc/method.h"
#include "../proc/type.h"
#include "../../user/syscall_num.h"

// from trampoline.S
extern char trampoline[];
extern char user_vector[];
extern void user_return(trapframe_t *tf, uint64 satp);

// 内核里本来就有的中断处理函数
extern void timer_interrupt_handler(void);
extern void external_interrupt_handler(void);

// -------------- user trap handler --------------
// 被 trampoline 的 user_vector 跳进来后最终 jr 到这里
// 不能直接 return，最后必须走 user_return 回到用户态
void trap_user_handler(void)
{
    proc_t *p = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_handler: null trapframe");

    // 读本次 trap 原因
    uint64 scause = r_scause();
    uint64 sepc   = r_sepc();
    uint64 is_intr = (scause >> 63) & 1;
    uint64 code    = scause & 0xfff;

    // 小调试：内核确认我们真的进来了
    *(volatile unsigned char *)UART_BASE = 'S';

    if (is_intr) {
        // =========== 异步中断 ===========
        // code 5 = STimer interrupt
        // code 9 = SExternal interrupt (PLIC / UART)
        // NOTE: 我们只是演示，不调度，不抢占，所以中断进来就简单处理下然后回去

        if (code == 5) {
            // S-mode timer interrupt
            timer_interrupt_handler();
        } else if (code == 9) {
            // S-mode external interrupt (UART)
            external_interrupt_handler();
        } else {
            printf("[usertrap] unexpected S interrupt code=%lu sepc=%lx\n",
                   code, sepc);
        }

        // 回用户时，从同一个 sepc 继续
        w_sepc(sepc);

    } else {
        // =========== 同步异常 ===========
        if (code == 8) {
            // U-mode ecall
            *(volatile unsigned char *)UART_BASE = 'E'; // ecall探针

            uint64 num = tf->a7;  // 约定: a7 = syscall number
            if (num == SYS_helloworld) {
                printf("proczero: hello world!\n");
            } else {
                printf("[user] unknown syscall %lu\n", num);
            }

            // ecall 触发时，陷阱的 sepc 指向那条 ecall 指令
            // 返回用户态时应当跳过它，否则会再次陷入
            w_sepc(sepc + 4);

        } else {
            printf("[usertrap] exception: scause=%lx code=%lu sepc=%lx stval=%lx\n",
                   scause, code, sepc, r_stval());
            panic("unhandled user exception");
        }
    }

    // ---- 准备 sret 回用户态 ----

    // 下一次从用户态进入内核时，trampoline 期望 sscratch 里是 tf
    w_sscratch((uint64)tf);

    // stvec 需要指向 user_vector (trampoline里的入口)，
    // 这样用户态再陷入会回到 trampoline
    w_stvec((uint64)user_vector);

    // sstatus：
    // - SPP=0 : sret 回到 U 模式
    // - SPIE=1: sret 后把 SIE 置 1（允许 S 中断在将来生效）
    uint64 sstatus = r_sstatus();
    sstatus &= ~SSTATUS_SPP;
    sstatus |=  SSTATUS_SPIE;
    w_sstatus(sstatus);

    // >>> 关键点 <<<
    // 我们不想在用户态立即再次被时钟/外部中断打断（现在还没实现调度）
    // 所以这里关掉 SIE 的 STIE/SEIE/SSIE 位
    uint64 sie = r_sie();
    sie &= ~(SIE_STIE | SIE_SEIE | SIE_SSIE);
    w_sie(sie);

    // 回去！
    user_return(tf, MAKE_SATP(p->pgtbl));
    __builtin_unreachable();
}


// -------------- first return from kernel to user --------------
// 这是 proczero.ctx.ra 指向的函数，第一次真正下到用户态会走这里
void trap_user_return(void)
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_return: null trapframe");

    // 打点：我们真的准备下用户态了
    *(volatile unsigned char *)UART_BASE = 'R';

    // sepc = 用户初始入口 (proc_make_first 里填的 user_to_kern_epc)
    w_sepc(tf->user_to_kern_epc);

    // stvec = trampoline 的 user_vector
    w_stvec((uint64)user_vector);

    // sscratch = 指向 trapframe
    w_sscratch((uint64)tf);

    // sstatus 设置成 “sret 会去 U, 并允许将来开中断”
    uint64 sstatus = r_sstatus();
    sstatus &= ~SSTATUS_SPP;   // 下一次 sret 回 U
    sstatus |=  SSTATUS_SPIE;  // sret 后允许 SIE=1
    w_sstatus(sstatus);

    // >>> 同样的关键点：先关 S 态的中断源位 <<<
    uint64 sie = r_sie();
    sie &= ~(SIE_STIE | SIE_SEIE | SIE_SSIE);
    w_sie(sie);

    // 也打点一下
    *(volatile unsigned char *)UART_BASE = 'U';

    // 切用户页表并 sret
    user_return(tf, MAKE_SATP(p->pgtbl));
    __builtin_unreachable();
}
