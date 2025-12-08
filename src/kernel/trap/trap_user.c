#include "mod.h"
#include "../arch/mod.h"
#include "../mem/mod.h"
#include "../proc/method.h"   // 提供进程/调度相关声明
#include "../fs/method.h"

// in trampoline.S
extern char trampoline[];   // 内核和用户切换的代码
extern char user_vector[];  // 用户触发陷阱进入内核（trampoline内偏移）
extern char user_return[];  // 内核处理完毕返回用户（trampoline内偏移）

// in trap.S
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口

// 来自 trap_kernel.c 的处理函数（有些工程未在公共头中声明，这里显式声明）
void timer_interrupt_handler(void);
void external_interrupt_handler(void);

// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
// 用户态 trap 的 C 处理逻辑（在 trampoline.S 的 user_vector 尾部调用）
void trap_user_handler()
{
    // 进入内核后，把 stvec 切到内核 trap 向量
    w_stvec((uint64)kernel_vector);

    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;

    // 记录用户 EPC
    tf->epc = r_sepc();

    uint64 scause = r_scause();
    int trap_id = scause & 0xf;

    if (scause & 0x8000000000000000UL) {
        // 中断（从 U 态进入）
        switch (trap_id) {
        case 1: // S-mode software interrupt（M 态时钟中断转发）
            timer_interrupt_handler();
            // 抢占：让出 CPU
            proc_yield();
            break;
        case 9: // S-mode external interrupt（PLIC）
            external_interrupt_handler();
            break;
        default:
            printf("unexpected user interrupt id=%d\n", trap_id);
            printf("sepc=%p stval=%p\n", tf->epc, r_stval());
            break;
        }
    } else {
        // 异常
        switch (trap_id) {
        case 8: // ecall from U-mode
            // 跳过 ecall 指令
            tf->epc += 4;
            syscall();
            break;
        case 13: // Load page fault
        case 15: // Store/AMO page fault
        {
            uint64 stval = r_stval();
            uint64 new_n = uvm_ustack_grow(p->pgtbl, p->ustack_npage, stval);
            if (new_n == (uint64)-1) {
                printf("ustack grow failed: npage=%p, stval=%p\n", p->ustack_npage, stval);
                panic("trap_user_handler");
            }
            p->ustack_npage = new_n;
            break;
        }
        default:
            printf("unexpected user exception id=%d sepc=%p stval=%p\n",
                   trap_id, tf->epc, r_stval());
            panic("trap_user_handler");
        }
    }

    trap_user_return();
}

void trap_user_return()
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;

    // 更新 hartid（供 trampoline 使用）
    tf->kernel_hartid = mycpuid();

    // stvec 指向用户态入口（trampoline 中的 user_vector）
    uint64 uservec_va = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(uservec_va);

    // sscratch 写 TRAPFRAME 虚拟地址（trampoline 约定）
    w_sscratch((uint64)TRAPFRAME);

    // sret 到 U：清 SPP（返回 U 态），置 SPIE（打开中断使能）
    uint64 s = r_sstatus();
    s &= ~SSTATUS_SPP;   // 下一次 sret 返回 U
    s |=  SSTATUS_SPIE;  // 使能中断
    w_sstatus(s);

    // 恢复用户 EPC
    w_sepc(tf->epc);

    // 跳到 trampoline 的 user_return(trapframe_va, user_satp)
    uint64 userret_va = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
    void (*ureturn)(trapframe_t *, uint64) = (void (*)(trapframe_t *, uint64))userret_va;
    ureturn((trapframe_t *)TRAPFRAME, MAKE_SATP(p->pgtbl));
}
