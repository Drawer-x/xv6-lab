#include "mod.h"
#include "../lock/type.h"  
#include "../lock/method.h"
#include "../arch/method.h" 
#include "../lib/mod.h"
#include "../proc/mod.h"  // 依赖 proc_sleep、proc_wakeup、mycpu

/*-------------------- 工作在M-mode --------------------*/

// in trap.S M-mode时钟中断处理流程()
extern void timer_vector();

// 每个CPU在时钟中断中需要的临时空间
static uint64 mscratch[NCPU][5] = {0};

void timer_init()
{
    // 获取当前cpuid
    int hartid = r_tp();

    // 设置初始值 cmp_time = cur_time + time_interval
    *(uint64*)CLINT_MTIMECMP(hartid) = *(uint64*)CLINT_MTIME + INTERVAL;

    // cur_mscratch 指向当前CPU的msrcatch数组
    uint64* cur_mscratch = mscratch[hartid];

    // cur_mscratch[1] [2] [3]先空着, 在trap.S里使用
    cur_mscratch[3] = CLINT_MTIMECMP(hartid); // cmp_time
    cur_mscratch[4] = INTERVAL;               // interval

    // 存放到临时寄存器, 便于与trap.S中的timer_vector协作
    w_mscratch((uint64)cur_mscratch);

    // 设置 M-mode 中断处理函数
    w_mtvec((uint64)timer_vector);

    // 打开 M-mode 中断总开关
    w_mstatus(r_mstatus() | MSTATUS_MIE);

    // 打开 M-mode 时钟中断分开关
    w_mie(r_mie() | MIE_MTIE);
}

/*--------------------- 工作在S-mode --------------------*/

// 全局系统时钟
static timer_t sys_timer[NCPU];

static struct {
    spinlock_t lk;
    uint64 total_ticks;  
} sys_total_timer;

void timer_create()
{
    int cpuid = r_tp();  

    // 每个CPU初始化自己的锁和计数
    spinlock_init(&sys_timer[cpuid].lk, "sys_timer");
    sys_timer[cpuid].ticks = 0;
    // 仅CPU0初始化全局总计数器（避免重复初始化）
    if (cpuid == 0) {
        spinlock_init(&sys_total_timer.lk, "sys_total_timer");
        sys_total_timer.total_ticks = 0;
    }
}

void timer_update()
{ 
    int cpuid = r_tp();

    // 更新全局总 ticks（原子操作）
    spinlock_acquire(&sys_total_timer.lk);
    sys_total_timer.total_ticks++;  
    spinlock_release(&sys_total_timer.lk);

    // 更新当前CPU的计数
    spinlock_acquire(&sys_timer[cpuid].lk);
    sys_timer[cpuid].ticks++; 
    spinlock_release(&sys_timer[cpuid].lk);

    // 更新当前CPU的mtimecmp（下一次时钟中断触发时间）
    volatile uint64 *mtime = (volatile uint64 *)CLINT_MTIME;
    volatile uint64 *mtimecmp = (volatile uint64 *)CLINT_MTIMECMP(cpuid);
    *mtimecmp = *mtime + INTERVAL;

    // 关键补充：唤醒所有等待时钟的进程（睡眠资源为全局系统时钟）
    proc_wakeup(&sys_total_timer);
}

uint64 timer_get_ticks()
{
    uint64 total = 0;
    spinlock_acquire(&sys_total_timer.lk);
    total = sys_total_timer.total_ticks;
    spinlock_release(&sys_total_timer.lk);
    return total;
}

// 让进程睡眠ntick个时钟周期（供sys_sleep系统调用调用）
void timer_wait(uint64 ntick)
{
    // 1. 合法性检查：ntick为0则直接返回（不睡眠）
    if (ntick == 0) {
        return;
    }

    // 2. 获取当前总 ticks，计算目标 ticks（当前 + ntick）
    uint64 target_ticks;
    spinlock_acquire(&sys_total_timer.lk);
    target_ticks = sys_total_timer.total_ticks + ntick;
    spinlock_release(&sys_total_timer.lk);

    // 3. 循环检查：未达到目标则睡眠，被唤醒后重新检查
    while (1) {
        spinlock_acquire(&sys_total_timer.lk);
        // 检查当前总 ticks 是否达到目标
        if (sys_total_timer.total_ticks >= target_ticks) {
            spinlock_release(&sys_total_timer.lk);
            break; // 达到目标，退出循环
        }
        // 未达到目标，睡眠等待（睡眠资源为全局系统时钟）
        // 注：proc_sleep会在睡眠前释放传入的锁，唤醒后重新获取
        proc_sleep(&sys_total_timer, &sys_total_timer.lk);
        // 被唤醒后，已重新持有sys_total_timer.lk，回到循环头部再次检查
    }
}

// -------------------------- 系统调用实现（sys_sleep）--------------------------
// 注：该函数需注册到系统调用表，供用户态调用
uint64 sys_sleep(uint64 ntick)
{
    // 1. 检查当前进程状态（必须是运行态）
    proc_t *p = mycpu()->proc;
    assert(p != NULL && p->state == RUNNING, "sys_sleep: not running");

    // 2. 调用timer_wait，让进程睡眠ntick个时钟周期
    timer_wait(ntick);

    // 3. 睡眠结束，返回0（成功）
    return 0;
}