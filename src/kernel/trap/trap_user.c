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
    w_sstatus(r_sstatus() | SSTATUS_SPIE);

    // sepc 已经在外面写好了
}

// 真正的 user trap handler（trampoline.S 的 user_vector 会跳到这里）
void trap_user_handler(void)
{
    uint64 scause = r_scause();
    uint64 sepc   = r_sepc();
    uint64 stval  = r_stval();   // 出错的地址（page fault 的时候用）

    uart_puts("[trap_user] scause=0x");
    uart_puthex(scause);
    uart_puts(" sepc=0x");
    uart_puthex(sepc);
    uart_puts(" stval=0x");
    uart_puthex(stval);
    uart_puts("\n");
    
    proc_t *p = myproc();
    uart_puts("[trap_user] proc tf=");
    uart_puthex((uint64)p->tf);
    uart_puts(" pgtbl=");
    uart_puthex((uint64)p->pgtbl);
    uart_puts("\n");

    if (scause & (1ULL << 63))
    {
        // -------------------------
        // 异步中断
        // -------------------------
        uint64 code = scause & 0xfff;
        if (code == 5 || code == 1)
        {
            // S-mode timer interrupt / software interrupt
            //uart_putc_sync('T');
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
            uart_puts("[usertrap] unexpected S interrupt code=");
            uart_putint(code);
            uart_puts("\n");
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

            // 添加系统调用号调试
            uint64 syscall_num = p->tf->a7;
            uart_puts("[usertrap] syscall ");
            uart_putint(syscall_num);
            uart_puts("\n");
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
                uart_puts("[usertrap] bad user stack grow: stval=");
                uart_puthex((uint64)(void *)stval);
                uart_puts("\n");
                panic("user stack grow failed");
            }
            p->ustack_npage = new_npage;

            // 继续从原来的指令执行
            w_sepc(sepc);
        }
        else
        {
            uart_puts("[usertrap] unhandled sync scause=0x");
            uart_puthex(scause);
            uart_puts(" sepc=0x");
            uart_puthex(sepc);
            uart_puts(" stval=0x");
            uart_puthex(stval);
            uart_puts("\n");
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
// 系统调用分发函数
void syscall() {
    proc_t *p = myproc();
    uint64 syscall_num = p->tf->a7; // 系统调用号从 a7 寄存器获取
    int ret = -1;

    switch (syscall_num) {
        case SYS_copyin:    // 假设 SYS_copyin 定义为 1
            ret = sys_copyin(p->tf->a0, p->tf->a1, p->tf->a2);
            break;
        case SYS_copyout:   // 假设 SYS_copyout 定义为 2
            ret = sys_copyout(p->tf->a0, p->tf->a1, p->tf->a2);
            break;
        case SYS_copyinstr: // 假设 SYS_copyinstr 定义为 3
            ret = sys_copyinstr(p->tf->a0, p->tf->a1, p->tf->a2);
            break;
        default:
            uart_puts("[syscall] unknown syscall: ");
            uart_putint(syscall_num);
            uart_puts("\n");
            ret = -1;
    }

    p->tf->a0 = ret; // 结果通过 a0 寄存器返回给用户态
}
// 检查用户虚拟地址范围 [va, va+len) 是否合法（属于用户可访问区域）
// 返回 0 表示合法，-1 表示非法
static int uvm_check_va(proc_t *p, uint64 va, uint64 len) {
    if (len == 0) return 0; // 空长度合法
    uint64 end = va + len;
    if (end < va) return -1; // 地址溢出

    // 1. 检查是否在用户空间基地址以下（无效）
    if (va < USER_BASE) return -1;

    // 2. 检查是否在用户栈区域
    uint64 stack_bot = TRAPFRAME - p->ustack_npage * PGSIZE;
    if (va >= stack_bot && end <= TRAPFRAME) {
        return 0;
    }

    // 3. 检查是否在用户堆区域（[USER_BASE, heap_top)）
    if (va >= USER_BASE && end <= p->heap_top) {
        return 0;
    }

    // 4. 检查是否在 mmap 区域
    mmap_region_t *region = p->mmap;
    while (region) {
        uint64 region_end = region->begin + region->npages * PGSIZE;
        if (va >= region->begin && end <= region_end) {
            return 0;
        }
        region = region->next;
    }

    // 不在任何合法区域
    return -1;
}
// 系统调用：sys_copyin
// 参数：
//   a0：用户空间源地址（user_src）
//   a1：内核空间目标地址（kern_dst）
//   a2：复制长度（len）
// 返回值：0 成功，-1 失败
int sys_copyin(uint64 user_src, uint64 kern_dst, uint32 len) {
    proc_t *p = myproc();
    if (p == NULL) return -1;

    // 1. 检查用户地址合法性
    if (uvm_check_va(p, user_src, len) != 0) {
        return -1;
    }

    // 2. 检查内核地址合法性（必须在内核空间）
    if (kern_dst < KERNEL_BASE || (kern_dst + len) < kern_dst) {
        return -1;
    }

    // 3. 调用 uvm_copyin 执行复制（需先修改 uvm_copyin 为有返回值）
    // 修改 uvm_copyin：若 uvm_va2pa 返回 0，返回 -1，否则返回 0
    if (uvm_copyin(p->pgtbl, kern_dst, user_src, len) != 0) {
        return -1;
    }

    return 0;
}
// 系统调用：sys_copyout
// 参数：
//   a0：内核空间源地址（kern_src）
//   a1：用户空间目标地址（user_dst）
//   a2：复制长度（len）
// 返回值：0 成功，-1 失败
int sys_copyout(uint64 kern_src, uint64 user_dst, uint32 len) {
    proc_t *p = myproc();
    if (p == NULL) return -1;

    // 1. 检查用户目标地址合法性
    if (uvm_check_va(p, user_dst, len) != 0) {
        return -1;
    }

    // 2. 检查内核源地址合法性
    if (kern_src < KERNEL_BASE || (kern_src + len) < kern_src) {
        return -1;
    }

    // 3. 调用 uvm_copyout 执行复制（需修改 uvm_copyout 为有返回值）
    if (uvm_copyout(p->pgtbl, user_dst, kern_src, len) != 0) {
        return -1;
    }

    return 0;
}
// 系统调用：sys_copyinstr
// 参数：
//   a0：用户空间字符串地址（user_src）
//   a1：内核空间缓冲区（kern_dst）
//   a2：最大复制长度（maxlen）
// 返回值：实际复制的字节数（不含终止符），-1 失败
int sys_copyinstr(uint64 user_src, uint64 kern_dst, uint32 maxlen) {
    proc_t *p = myproc();
    if (p == NULL || maxlen == 0) return -1;

    // 1. 检查用户地址合法性（最大可能访问 maxlen 字节）
    if (uvm_check_va(p, user_src, maxlen) != 0) {
        return -1;
    }

    // 2. 检查内核缓冲区合法性
    if (kern_dst < KERNEL_BASE || (kern_dst + maxlen) < kern_dst) {
        return -1;
    }

    // 3. 调用 uvm_copyin_str 复制字符串（需修改为返回实际长度，错误时返回-1）
    int copied = uvm_copyin_str(p->pgtbl, kern_dst, user_src, maxlen);
    return copied;
}