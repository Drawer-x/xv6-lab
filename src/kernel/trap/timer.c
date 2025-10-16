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

// timer.c - 正常的timer_init函数
void timer_init()
{
    // 获取当前cpuid
    int hartid = r_tp();
    printf("timer_init: hartid=%d, mscratch[hartid]=%p\n", 
           hartid, &mscratch[hartid]);
    // 设置初始值 cmp_time = cur_time + time_interval
    *(uint64*)CLINT_MTIMECMP(hartid) = *(uint64*)CLINT_MTIME + INTERVAL;

    // cur_mscratch 指向当前CPU的mscratch数组
    volatile uint64* cur_mscratch = mscratch[hartid];

    //if (cur_mscratch == 0) {
    //    return;
    //}

    // cur_mscratch[1] [2] [3]先空着, 在trap.S里使用
    cur_mscratch[3] = CLINT_MTIMECMP(hartid); // cmp_time
    cur_mscratch[4] = INTERVAL;               // interval

    // 存放到临时寄存器, 便于与trap.S中的timer_vector协作
    w_mscratch((uint64)cur_mscratch);
    // 【新增日志】验证绑定结果（读取寄存器确认）
    uint64_t mscratch_val = r_mscratch();  // 读取mscratch寄存器值
    printf("[timer_init] hartid=%d: mscratch register = %p (expected %p)\n",
           hartid, (void*)mscratch_val, (void*)cur_mscratch);

    // 【新增验证】若绑定失败，强制报错（便于调试）
    if (mscratch_val != (uint64_t)cur_mscratch) {
        printf("[ERROR] hartid=%d: mscratch绑定失败！实际=%p 预期=%p\n",
               hartid, (void*)mscratch_val, (void*)cur_mscratch);
        // 触发断点（供GDB捕获）
        asm volatile("ebreak");
    }
    // 设置 M-mode 中断处理函数
    w_mtvec((uint64)timer_vector);

    // 打开 M-mode 中断总开关
    w_mstatus(r_mstatus() | MSTATUS_MIE);

    // 打开 M-mode 时钟中断分开关
    w_mie(r_mie() | MIE_MTIE);
}
/*--------------------- 工作在S-mode --------------------*/

// 全局系统时钟
static timer_t sys_timer;

// 时钟创建
void timer_create()
{
    // 初始化自旋锁（命名为"sys_timer"，便于调试）
    spinlock_init(&sys_timer.lk, "sys_timer");
    // 初始滴答数为0（内核启动时时间从0开始）
    sys_timer.ticks = 0;

}

// 时钟更新
void timer_update()
{
    // 加自旋锁：确保多CPU核心并发时，ticks修改是原子操作
    spinlock_acquire(&sys_timer.lk);
    
    // 滴答数+1（每INTERVAL个cycle对应1个tick，即0.1秒）
    sys_timer.ticks++;
    // 核心：按 CPU ID 输出 "di da"
    int cpuid = r_tp(); // 获取当前 CPU 核心 ID
    printf("cpu %d:di da\n", cpuid);
    
    // 解锁：允许其他核心访问ticks
    spinlock_release(&sys_timer.lk);

}

// 获取滴答数量 (不把sys_timer暴露出去, 只提供安全的访问接口)
uint64 timer_get_ticks()
{
    uint64 ticks;  // 临时变量：存储读取到的ticks（避免返回时被修改）

    // 加锁：确保读取过程中ticks不被其他核心修改
    spinlock_acquire(&sys_timer.lk);
    
    ticks = sys_timer.ticks;  // 读取当前滴答数
    
    // 解锁：释放对sys_timer的占用
    spinlock_release(&sys_timer.lk);

    return ticks;  // 返回读取到的安全值

}