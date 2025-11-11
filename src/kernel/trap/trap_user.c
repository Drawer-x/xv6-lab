#include "mod.h"
#include "../mem/mod.h"
#include "../lib/method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../proc/method.h"
#include "../proc/type.h"
#include "../arch/method.h"
#include "../arch/mod.h"
#include "../../user/syscall_num.h"

// 外部符号声明
extern char trampoline[];         // 内核-用户切换跳板
extern char user_vector[];        // 用户态陷阱入口（trampoline内偏移）
extern void timer_interrupt_handler(void);  // 定时器中断处理
extern void external_interrupt_handler(void); // 外部中断处理
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口
extern void user_return(trapframe_t *tf, uint64 user_satp);

// static void return_to_user(proc_t *p)
// {
//     uart_puts("[return_to_user] START\n");  // 先确认函数是否执行到这里
    
//     trapframe_t *tf = p->tf;
//     assert(tf != NULL, "return_to_user: null trapframe");

//     uart_puts("[return_to_user] tf ok\n");
//     // 1. 提前计算用户态陷阱入口（但不立即设置 stvec）
//     uint64 uservec_va = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);

//     // 2. 计算用户页表 satp 等变量（供输出使用）
//     uint64 user_satp = MAKE_SATP(p->pgtbl);
//     uint64 current_sstatus = r_sstatus();  // 提前保存当前 sstatus

//     // 3. 执行所有内核态输出（此时 stvec 仍为内核态入口 kernel_vector）
//     printf("?");
//     printf("[return_to_user] user_satp=0x%lx\n", user_satp);
//     printf("?");
//     printf("[return_to_user] sstatus=0x%lx (expected SPP=0, SPIE=1)\n", current_sstatus);
//     printf("[return_to_user] calling user_return, tf=0x%lx, user_satp=0x%lx\n", tf, user_satp);

//     // 4. 所有输出完成后，再设置用户态相关寄存器（包括 stvec）
//     w_sscratch((uint64)TRAPFRAME);         // 设置用户态 scratch
//     w_stvec(uservec_va);                   // 延迟设置 stvec 为用户态入口
//     // 配置 sstatus（允许中断 + 用户态）
//     uint64 sstatus = current_sstatus;
//     sstatus |= SSTATUS_SPIE;
//     sstatus &= ~SSTATUS_SPP;
//     w_sstatus(sstatus);

//     // 5. 跳转用户态
//     user_return(tf, user_satp);
// }
static void return_to_user(proc_t *p)
{
    uart_puts("[return_to_user] START\n");
    
    trapframe_t *tf = p->tf;
    uart_puts("[return_to_user] tf ok\n");
    
    // 1. 设置 sscratch
    w_sscratch((uint64)TRAPFRAME);
    uart_puts("[return_to_user] sscratch set to ");
    uart_puthex((uint64)TRAPFRAME);
    uart_puts("\n");
    
    // 2. 设置 stvec
    uint64 uservec_va = TRAMPOLINE;
    uart_puts("[return_to_user] setting stvec to ");
    uart_puthex(uservec_va);
    uart_puts("\n");
    
    w_stvec(uservec_va);
    
    // 验证 stvec 是否设置成功
    uint64 current_stvec = r_stvec();
    uart_puts("[return_to_user] current stvec=");
    uart_puthex(current_stvec);
    uart_puts("\n");
    
    if (current_stvec != uservec_va) {
        uart_puts("[return_to_user] ERROR: stvec not set correctly!\n");
        return;
    }
    uart_puts("[return_to_user] stvec set correctly\n");
    
    // 3. 设置 sstatus
    uint64 sstatus = r_sstatus();
    uart_puts("[return_to_user] old sstatus=");
    uart_puthex(sstatus);
    uart_puts(", SPP=");
    uart_puthex((sstatus & SSTATUS_SPP) >> 8);
    uart_puts(", SPIE=");
    uart_puthex((sstatus & SSTATUS_SPIE) >> 5);
    uart_puts("\n");
    
    sstatus |= SSTATUS_SPIE;
    sstatus &= ~SSTATUS_SPP;
    w_sstatus(sstatus);
    
    uint64 new_sstatus = r_sstatus();
    uart_puts("[return_to_user] new sstatus=");
    uart_puthex(new_sstatus);
    uart_puts(", SPP=");
    uart_puthex((new_sstatus & SSTATUS_SPP) >> 8);
    uart_puts(", SPIE=");
    uart_puthex((new_sstatus & SSTATUS_SPIE) >> 5);
    uart_puts(" (should be SPP=0, SPIE=1)\n");
    
    // 4. 准备调用 user_return
    uint64 user_satp = MAKE_SATP(p->pgtbl);
    uart_puts("[return_to_user] user_satp=");
    uart_puthex(user_satp);
    uart_puts("\n");
    
    uart_puts("[return_to_user] calling user_return with tf=");
    uart_puthex((uint64)tf);
    uart_puts(", user_satp=");
    uart_puthex(user_satp);
    uart_puts("\n");
    
    user_return(tf, user_satp);
    
    uart_puts("[return_to_user] ERROR: returned from user_return\n");
}
// 用户态陷阱处理核心逻辑（trampoline的user_vector会跳转至此）
void trap_user_handler(void)
{
    // 1. 进入内核时切换中断向量到内核态处理逻辑
    uint64 old_stvec = r_stvec();  // 保存原向量，便于异常流程恢复
    w_stvec((uint64)kernel_vector);

    // 2. 读取陷阱状态寄存器
    uint64 scause = r_scause();
    uint64 sepc = r_sepc();
    uint64 stval = r_stval();
    printf("[trap_user] scause=0x%lx, sepc=0x%lx, stval=0x%lx\n", scause, sepc, stval);
    proc_t *p = myproc();
    trapframe_t *tf = p->tf;
    assert(p != NULL && tf != NULL, "trap_user_handler: invalid proc/trapframe");

    // 3. 持久化保存用户态陷阱发生时的PC到陷阱帧
    tf->user_to_kern_epc = sepc;

    // 4. 打印基础调试信息
    uart_puts("[trap_user] scause=0x");
    uart_puthex(scause);
    uart_puts(" (");
    if (scause & (1ULL << 63)) {
        uart_puts("interrupt");
    } else {
        uart_puts("exception");
    }
    uart_puts(") sepc=0x");
    uart_puthex(sepc);
    uart_puts(" stval=0x");
    uart_puthex(stval);
    uart_puts("\n");

    // 5. 处理中断/异常
    if (scause & (1ULL << 63)) {
        // 异步中断处理
        uint64 trap_id = scause & 0xfff;

        switch (trap_id) {
            case 1:  // S-mode软件中断（定时器转发）
                timer_interrupt_handler();
                break;
            case 9:  // S-mode外部中断（PLIC）
                external_interrupt_handler();
                break;
            default:
                // 未识别中断：容错处理，不panic
                uart_puts("[trap_user] unhandled interrupt, skip panic\n");
                break;
        }
        // 恢复用户态PC（中断不改变原执行流程）
        w_sepc(tf->user_to_kern_epc);
    } else {
        // 同步异常处理
        uint64 trap_id = scause & 0xfff;

        switch (trap_id) {
            case 8:  // 用户态系统调用（ecall）
                // 系统调用返回地址为ecall下一条指令
                tf->user_to_kern_epc += 4;
                w_sepc(tf->user_to_kern_epc);
                // 打印系统调用号调试信息
                uart_puts("[trap_user] syscall num=");
                uart_putint(tf->a7);
                uart_puts("\n");
                syscall();  // 分发系统调用
                break;
            case 13:  // 加载页错误
            case 15:  // 存储/AMO页错误
                // 尝试自动扩展用户栈
                uint64 old_npage = p->ustack_npage;
                uint64 new_npage = uvm_ustack_grow(p->pgtbl, old_npage, stval);
                if (new_npage == (uint64)-1) {
                    uart_puts("[trap_user] stack grow failed! stval=0x");
                    uart_puthex(stval);
                    uart_puts("\n");
                    panic("user stack out of memory");
                }
                // 打印栈扩展信息
                uart_puts("[trap_user] stack grow: ");
                uart_putint(old_npage);
                uart_puts(" -> ");
                uart_putint(new_npage);
                uart_puts(" pages\n");
                p->ustack_npage = new_npage;
                // 恢复原PC，重试指令
                w_sepc(tf->user_to_kern_epc);
                break;
            default:
                // 未识别异常：容错处理，不panic
                uart_puts("[trap_user] unhandled exception, skip panic\n");
                w_sepc(tf->user_to_kern_epc);
                break;
        }
    }

    // 6. 恢复内核态中断向量（若需要），返回用户态
    w_stvec(old_stvec);  // 恢复进入时的stvec（兼容异常流程）
    return_to_user(p);
}

// 首次进入用户态的初始化处理
void trap_user_return(void)
{
    proc_t *p = myproc();
    trapframe_t *tf = p->tf;
    assert(p != NULL && tf != NULL, "trap_user_return: invalid proc/trapframe");

    uart_puts("[trap_user_return] first enter user, sepc=0x");
    uart_puthex(tf->user_to_kern_epc);
    uart_puts("\n");

    // stvec -> 用户向量（高地址）
    uint64 uservec_va = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(uservec_va);
    uart_puts("[trap_user_return] stvec set to uservec_va=0x");
    uart_puthex(uservec_va);
    uart_puts("\n");

    // sscratch 写入 TRAPFRAME 虚拟地址
    w_sscratch((uint64)TRAPFRAME);
    uart_puts("[trap_user_return] sscratch set to TRAPFRAME=0x");
    uart_puthex((uint64)TRAPFRAME);
    uart_puts("\n");

    // sret 到 U：清 SPP，置 SPIE
    uint64 s = r_sstatus();
    s &= ~SSTATUS_SPP;
    s |= SSTATUS_SPIE;
    w_sstatus(s);
    uart_puts("[trap_user_return] sstatus set for user mode\n");

    // 设置 sepc
    w_sepc(tf->user_to_kern_epc);
    uart_puts("[trap_user_return] sepc set to 0x");
    uart_puthex(tf->user_to_kern_epc);
    uart_puts("\n");

    // 跳转 trampoline 的 user_return(trapframe_va, user_satp)
    uint64 userret_va = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
    uart_puts("[trap_user_return] calling user_return at va=0x");
    uart_puthex(userret_va);
    uart_puts(" with tf=TRAPFRAME, user_satp=");
    uart_puthex(MAKE_SATP(p->pgtbl));
    uart_puts("\n");

    void (*ureturn)(trapframe_t *, uint64) = (void (*)(trapframe_t *, uint64))userret_va;
    ureturn((trapframe_t *)TRAPFRAME, MAKE_SATP(p->pgtbl));

    // 不应该返回到这里
    uart_puts("[trap_user_return] ERROR: returned from user_return!\n");
}