// src/kernel/trap/trap_user.c
#include "mod.h"
#include "../lib/method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../proc/method.h"
#include "../proc/type.h"
#include "../arch/method.h"
#include "../../user/syscall_num.h"

// from trampoline.S
extern char trampoline[];
extern char user_vector[];
extern void user_return(trapframe_t *tf, uint64 satp);

// S-mode interrupt helpers (already defined elsewhere)
extern void timer_interrupt_handler(void);
extern void external_interrupt_handler(void);

// 内部工具：从内核返回用户
static void return_to_user(proc_t *p)
{
    trapframe_t *tf = p->tf;

    // sscratch = trapframe
    w_sscratch((uint64)tf);

    // stvec = trampoline + (user_return - trampoline)    (在 trampoline 里算的)
    uint64 trampoline_pa = (uint64)trampoline;
    uint64 user_ret_off  = (uint64)user_return - (uint64)trampoline;
    w_stvec(trampoline_pa + user_ret_off);

    // 切回用户页表
    w_satp(MAKE_SATP(p->pgtbl));
    sfence_vma();

    // 开中断 + 保留 S 态中断使能
    w_sstatus(r_sstatus() | SSTATUS_SPIE | SSTATUS_SUM);

    // sepc 已经在外面写好了
}

// 真正的 user trap handler（trampoline.S 的 user_vector 会跳到这里）
void trap_user_handler(void)
{
    uint64 scause = r_scause();
    uint64 sepc   = r_sepc();
    uint64 stval  = r_stval();   // 出错的地址（page fault 的时候用）

    proc_t *p = myproc();
    trapframe_t *tf = p->tf;

    if (scause & (1ULL << 63))
    {
        // -------------------------
        // 异步中断
        // -------------------------
        uint64 code = scause & 0xfff;
        if (code == 5 || code == 1)
        {
            // S-mode timer interrupt / software interrupt
            uart_putc_sync('T');
            timer_interrupt_handler();

            // 回到原PC
            w_sepc(sepc);
        }
        else if (code == 9)
        {
            // 外设中断
            external_interrupt_handler();
            w_sepc(sepc);
        }
        else
        {
            printf("[usertrap] unexpected S interrupt code=%d\n", (int)code);
            w_sepc(sepc);
        }
    }
    else
    {
        // -------------------------
        // 同步异常
        // -------------------------
        uint64 code = scause & 0xfff;
        if (code == 8)
        {
            // ecall from U
            uart_putc_sync('E');

            // ecall 会让 sepc 指向 ecall 本身，要在进 sys 前 +4
            w_sepc(sepc + 4);

            // 交给通用 syscall 分发
            syscall();
        }
        else if (code == 13 || code == 15)
        {
            // load/store/AMO page fault -> 可能是用户栈要自动增长
            uint64 new_npage = uvm_ustack_grow(p->pgtbl, p->ustack_npage, stval);
            if (new_npage == (uint64)-1)
            {
                printf("[usertrap] bad user stack grow: stval=%p\n", (void *)stval);
                panic("user stack grow failed");
            }
            p->ustack_npage = new_npage;

            // 继续从原来的指令执行
            w_sepc(sepc);
        }
        else
        {
            printf("[usertrap] unhandled sync scause=0x%lx sepc=0x%lx stval=0x%lx\n",
                   scause, sepc, stval);
            panic("unhandled user exception");
        }
    }

    // 处理完了回用户
    return_to_user(p);
}

// 第一次进用户态
void trap_user_return(void)
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_return: null trapframe");

    // 第一次的 sepc 是进程创建时写进去的
    uint64 entry = tf->user_to_kern_epc;
    w_sepc(entry);

    return_to_user(p);
}
