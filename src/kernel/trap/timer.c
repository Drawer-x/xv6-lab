#include "mod.h"
#include "../lock/type.h"  
#include "../lock/method.h"
#include "../arch/method.h" 
#include "../lib/mod.h"

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

    spinlock_acquire(&sys_total_timer.lk);
    sys_total_timer.total_ticks++;  
    spinlock_release(&sys_total_timer.lk);

    // 更新当前CPU的计数
    spinlock_acquire(&sys_timer[cpuid].lk);
    sys_timer[cpuid].ticks++;
    // printf中直接使用全局总计数
    printf("cpu %d:di da (total ticks: %d) | 全局总计数: %d\n", 
        cpuid, 
        (int)sys_timer[cpuid].ticks, 
        timer_get_ticks());  
    spinlock_release(&sys_timer[cpuid].lk);

    // 更新当前CPU的mtimecmp
    volatile uint64 *mtime = (volatile uint64 *)CLINT_MTIME;
    volatile uint64 *mtimecmp = (volatile uint64 *)CLINT_MTIMECMP(cpuid);
    *mtimecmp = *mtime + INTERVAL;
}

uint64 timer_get_ticks()
{
    uint64 total = 0;
    spinlock_acquire(&sys_total_timer.lk);
    total = sys_total_timer.total_ticks;
    spinlock_release(&sys_total_timer.lk);
    return total;
}