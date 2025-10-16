#include "mod.h"
#include "../lock/type.h"  
#include "../lock/method.h"
#include "../arch/method.h" 
#include "../lib/mod.h"
#include <stdint.h>  // 定义 uint64_t 等标准整数类型
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
    printf("[DEBUG] timer_init done for cpu%d\n", r_tp());

}

/*--------------------- 工作在S-mode --------------------*/

// 全局系统时钟
static timer_t sys_timer;

// 时钟创建
void timer_create()
{
    // 关键：仅 CPU0 执行定时器初始化，CPU1 直接返回
    if (r_tp() != 0) {
        printf("[DEBUG] timer_create: CPU1 skip (only CPU0 init)\n");
        return;
    }

    // 仅 CPU0 初始化锁和计数
    spinlock_init(&sys_timer.lk, "sys_timer");
    sys_timer.ticks = 0;
    timer_update();  // CPU0 调用，配置首次 mtimecmp
    printf("[DEBUG] timer_init: done (CPU%d)\n", r_tp());  // 此时仅 CPU0 打印
}

void timer_update()
{
    printf("Enter timer_update\n"); 
    int cpuid = r_tp();

    // 关键：仅 CPU0 处理锁和计数（避免多 CPU 锁竞争）
    if (cpuid == 0) {
        // 加锁前先检查锁状态（可选，调试用）
        if (spinlock_holding(&sys_timer.lk)) {
            printf("[DEBUG] CPU%d: already holding timer lock\n", cpuid);
        } else {
            spinlock_acquire(&sys_timer.lk);  // 仅 CPU0 加锁
            sys_timer.ticks++;
            // 修复格式化字符串：用 %d 替代 %llu（若 printf 不支持 64 位格式）
            printf("cpu %d:di da (total ticks: %d)\n", cpuid, (int)sys_timer.ticks);
            spinlock_release(&sys_timer.lk);  // 仅加锁的 CPU0 释放锁
        }
    }

    // 配置 mtimecmp（仅 CPU0 负责，与之前一致）
    if (cpuid == 0) {
        volatile uint64_t *mtime = (volatile uint64_t *)CLINT_MTIME;
        volatile uint64_t *mtimecmp = (volatile uint64_t *)CLINT_MTIMECMP(cpuid);
        *mtimecmp = *mtime + INTERVAL;
        // 修复格式化字符串
        printf("[DEBUG] CPU0: mtime=%d, next mtimecmp=%d\n", (int)*mtime, (int)*mtimecmp);
    }
}

// 获取滴答数量 (不把sys_timer暴露出去, 只提供安全的访问接口)
uint64 timer_get_ticks()
{
    uint64 ticks; 
    spinlock_acquire(&sys_timer.lk);
    ticks = sys_timer.ticks; 
    spinlock_release(&sys_timer.lk);
    return ticks;
}