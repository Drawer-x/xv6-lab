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
extern char user_return[];        // 内核返回用户态入口（trampoline内偏移）
extern void timer_interrupt_handler(void);  // 定时器中断处理
extern void external_interrupt_handler(void); // 外部中断处理
extern char *interrupt_info[16];  // 中断类型描述信息
extern char *exception_info[16];  // 异常类型描述信息
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口
// 内部工具：从内核返回用户态
static void return_to_user(proc_t *p)
{
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "return_to_user: null trapframe");

    // 设置sscratch为全局TRAPFRAME（与内核陷阱帧管理一致）
    w_sscratch((uint64)TRAPFRAME);

    // 设置用户态陷阱入口：trampoline中的user_vector
    uint64 uservec_va = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
    w_stvec(uservec_va);

    // 切换到用户页表
    w_satp(MAKE_SATP(p->pgtbl));
    sfence_vma();  // 刷新TLB，确保页表切换生效

    // 允许用户态中断（置位SPIE），清除SPP（标记返回用户态）
    uint64 sstatus = r_sstatus();
    sstatus |= SSTATUS_SPIE;  // 开中断
    sstatus &= ~SSTATUS_SPP;  // 回到用户态
    w_sstatus(sstatus);
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
        uart_puts("[trap_user] interrupt id=");
        uart_putint(trap_id);
        uart_puts(" ");
        if (trap_id < 16 && interrupt_info[trap_id]) {
            uart_puts(interrupt_info[trap_id]);  // 打印中断描述
        } else {
            uart_puts("unknown interrupt");
        }
        uart_puts("\n");

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
        uart_puts("[trap_user] exception id=");
        uart_putint(trap_id);
        uart_puts(" ");
        if (trap_id < 16 && exception_info[trap_id]) {
            uart_puts(exception_info[trap_id]);  // 打印异常描述
        } else {
            uart_puts("unknown exception");
        }
        uart_puts("\n");

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

    // 初始化用户态入口PC（从陷阱帧读取）
    w_sepc(tf->user_to_kern_epc);
    uart_puts("[trap_user_return] first enter user, sepc=0x");
    uart_puthex(tf->user_to_kern_epc);
    uart_puts("\n");

    return_to_user(p);
}