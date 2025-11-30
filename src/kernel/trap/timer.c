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
// 全局系统时钟（保留多CPU数组）
static timer_t sys_timer[NCPU];

// 全局总计数器（保留，用于跨CPU统一计时）
static struct {
    spinlock_t lk;
    uint64 total_ticks;  
} sys_total_timer;

// 时钟创建（保留多CPU初始化逻辑）
void timer_create()
{
    int cpuid = r_tp();  

    // 每个CPU初始化自己的锁和计数
    spinlock_init(&sys_timer[cpuid].lk, "sys_timer");
    sys_timer[cpuid].ticks = 0;
    
    // 仅CPU0初始化全局总计数器（保留原有逻辑）
    if (cpuid == 0) {
        spinlock_init(&sys_total_timer.lk, "sys_total_timer");
        sys_total_timer.total_ticks = 0;
    }
}

// 时钟更新（保留多CPU逻辑，对齐唤醒/锁风格）
void timer_update()
{ 
    int cpuid = r_tp();

    // 更新全局总 ticks（保留原子操作）
    spinlock_acquire(&sys_total_timer.lk);
    sys_total_timer.total_ticks++;  
    spinlock_release(&sys_total_timer.lk);

    // 更新当前CPU的计数（保留per-CPU逻辑）
    spinlock_acquire(&sys_timer[cpuid].lk);
    sys_timer[cpuid].ticks++; 
    spinlock_release(&sys_timer[cpuid].lk);

    // 更新当前CPU的mtimecmp（保留per-CPU中断配置）
    volatile uint64 *mtime = (volatile uint64 *)CLINT_MTIME;
    volatile uint64 *mtimecmp = (volatile uint64 *)CLINT_MTIMECMP(cpuid);
    *mtimecmp = *mtime + INTERVAL;

    // 关键调整：唤醒资源改为全局总计时器（与timer_wait对齐）
    proc_wakeup(&sys_total_timer);
}

// 获取滴答数量（保留原有接口，返回全局总ticks）
uint64 timer_get_ticks()
{
    uint64 total = 0;
    spinlock_acquire(&sys_total_timer.lk);
    total = sys_total_timer.total_ticks;
    spinlock_release(&sys_total_timer.lk);
    return total;
}

// 让进程睡眠ntick个时钟周期（对齐参考代码逻辑，保留多CPU计时）
void timer_wait(uint64 ntick)
{
    // 1. 合法性检查：ntick为0则直接返回（保留原有逻辑）
    if (ntick == 0) {
        return;
    }

    // 2. 获取当前总 ticks，计算目标 ticks（对齐参考代码锁风格）
    spinlock_acquire(&sys_total_timer.lk);
    uint64 target_ticks = sys_total_timer.total_ticks + ntick;

    // 3. 循环检查：未达到目标则睡眠（完全对齐参考代码逻辑）
    while (sys_total_timer.total_ticks < target_ticks) {
        printf("proc %d is sleeping!\n", myproc()->pid, target_ticks);
        // 以全局总计时器为资源睡眠，自动释放/重获取锁
        proc_sleep(&sys_total_timer, &sys_total_timer.lk);
        // 被唤醒后已重新持有sys_total_timer.lk，直接循环检查
    }
    printf("proc %d is wakeup!\n", myproc()->pid, sys_total_timer.total_ticks);

    // 4. 释放全局锁（对齐参考代码，最后统一释放）
    spinlock_release(&sys_total_timer.lk);
}