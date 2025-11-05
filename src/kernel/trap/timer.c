#include "mod.h"
#include "../lock/type.h"
#include "../lock/method.h"
#include "../arch/method.h"
#include "../lib/mod.h"

// -------------------- M-mode timer side --------------------

// from trap.S
extern void timer_vector();

// each hart's mscratch scratchpad for M-mode timer_vector
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


// -------------------- S-mode accounting side --------------------

static timer_t sys_timer[NCPU];

static struct {
    spinlock_t lk;
    uint64 total_ticks;
} sys_total_timer;

void timer_create()
{
    int cpuid = r_tp();

    // per-hart counter init
    spinlock_init(&sys_timer[cpuid].lk, "sys_timer");
    sys_timer[cpuid].ticks = 0;

    // only hart0 init global counter/lock
    if (cpuid == 0) {
        spinlock_init(&sys_total_timer.lk, "sys_total_timer");
        sys_total_timer.total_ticks = 0;
    }
}

// this is called from trap_kernel_handler() for timer/SSIP
void timer_interrupt_handler(void)
{
    int cpuid = r_tp();

    // bump global ticks
    spinlock_acquire(&sys_total_timer.lk);
    sys_total_timer.total_ticks++;
    //uint64 now_total = sys_total_timer.total_ticks;
    spinlock_release(&sys_total_timer.lk);

    // bump per-cpu ticks
    spinlock_acquire(&sys_timer[cpuid].lk);
    sys_timer[cpuid].ticks++;
    //uint64 hart_ticks = sys_timer[cpuid].ticks;
    spinlock_release(&sys_timer[cpuid].lk);
    w_sip(r_sip() & ~2ULL);
}

// helper if you ever want to read total_ticks elsewhere
uint64 timer_get_ticks()
{
    uint64 total;
    spinlock_acquire(&sys_total_timer.lk);
    total = sys_total_timer.total_ticks;
    spinlock_release(&sys_total_timer.lk);
    return total;
}
