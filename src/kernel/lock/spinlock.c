#include "mod.h"

/*
    开关中断的基本逻辑:
    1. 多个地方可能开关中断, 因此它不是“开/关”的二元状态，而是“关 关 关 开 开 开”的stack
    2. 在第一次执行关中断时, 记录中断的初始状态为X
    3. 每次关中断, stack中的元素加1
    4. 每次开中断, stack中的元素减1
    5. 如果stack中元素清空, 将中断状态设为初始的X
*/

// 带层数叠加的关中断
void push_off(void)
{
    int old = intr_get();
    intr_off();
    cpu_t *cpu = mycpu();
    if (cpu->noff == 0)
        cpu->origin = old;
    cpu->noff++;
}

// 带层数叠加的开中断
void pop_off(void)
{
    cpu_t *cpu = mycpu();
    assert(intr_get() == 0, "push_off: 1\n"); // 确保此时中断是关闭的
    assert(cpu->noff >= 1, "push_off: 2\n");  // 确保push和pop的对应
    cpu->noff--;
    if (cpu->noff == 0 && cpu->origin == 1) // 只有所有push操作都被抵消且原来状态是开着时
        intr_on();
}


// 自选锁初始化
void spinlock_init(spinlock_t *lk, char *name)
{
    lk->locked = 0;
    lk->name = name;
    lk->cpuid = -1;  // 使用-1表示未持有，避免与0号CPU冲突
}

// 是否持有自旋锁
bool spinlock_holding(spinlock_t *lk)
{
    return lk->locked && lk->cpuid == mycpuid();
}

// 获取自选锁
// 获取自旋锁（带锁名定位）
void spinlock_acquire(spinlock_t *lk)
{
    push_off(); // 关中断

    if (spinlock_holding(lk)) {
        // ===== 诊断日志开始（修正版：不访问 c->id）=====
        int cpuid = mycpuid();
        cpu_t *c = mycpu();
        proc_t *p = c ? c->proc : NULL;
        int pid = p ? p->pid : -1;
        int pstate = p ? p->state : -1;

        uint64 sstatus = r_sstatus();
        uint64 sie = r_sie();
        uint64 sip = r_sip();

        uart_puts("spinlock_acquire: already holding: ");
        if (lk->name) uart_puts(lk->name); else uart_puts("(noname)");
        uart_puts("\n");

        uart_puts("  cpu="); uart_puthex(cpuid); uart_puts("\n");
        uart_puts("  myproc.pid="); uart_puthex(pid);
        uart_puts(" state="); uart_puthex(pstate); uart_puts("\n");
        uart_puts("  lk->cpuid="); uart_puthex(lk->cpuid);
        uart_puts(" locked="); uart_puthex(lk->locked); uart_puts("\n");
        uart_puts("  sstatus="); uart_puthex(sstatus);
        uart_puts(" sie="); uart_puthex(sie);
        uart_puts(" sip="); uart_puthex(sip); uart_puts("\n");
        // ===== 诊断日志结束 =====

        panic("spinlock_acquire: already holding");
    }

    while (__sync_lock_test_and_set(&lk->locked, 1) != 0) {
        // 自旋
    }
    __sync_synchronize();
    lk->cpuid = mycpuid();
}




// 释放自旋锁
void spinlock_release(spinlock_t *lk)
{
    int cpuid = mycpuid();

    // 在修改任何状态之前检查是否持有锁
    if (lk->cpuid != cpuid || lk->locked == 0) {
        // 详细诊断：哪把锁、当前 CPU、锁属主、锁位、以及中断相关寄存器
        printf("spinlock_release: not holding: %s\n", lk->name ? lk->name : "(noname)");
        printf("  cpu=%d\n", cpuid);
        proc_t *p = myproc();
        if (p) {
            printf("  myproc.pid=%d state=%d\n", p->pid, p->state);
        } else {
            printf("  myproc.pid=<none>\n");
        }
        printf("  lk->cpuid=%d locked=%d\n", lk->cpuid, lk->locked);
        printf("  sstatus=%p sie=%p sip=%p\n", r_sstatus(), r_sie(), r_sip());
        panic("spinlock_release: not holding");
    }

    lk->cpuid = -1;  // 重置为-1表示未持有

    // 插入内存屏障
    __sync_synchronize();

    // 原子释放锁
    __sync_lock_release(&lk->locked);

    pop_off(); // 开中断（按嵌套计数）
}
