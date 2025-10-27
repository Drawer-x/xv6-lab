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
    int hartid = r_tp();

    volatile uint64 *mtime    = (volatile uint64 *)CLINT_MTIME;
    volatile uint64 *mtimecmp = (volatile uint64 *)CLINT_MTIMECMP(hartid);
    *mtimecmp = *mtime + INTERVAL;

    uint64 *cur = mscratch[hartid];
    cur[3] = (uint64)mtimecmp;
    cur[4] = INTERVAL;

    w_mscratch((uint64)cur);
    w_mtvec((uint64)timer_vector);

    w_mie(r_mie() | MIE_MTIE);
    w_mstatus(r_mstatus() | MSTATUS_MIE);

    // ✅ 打印调试信息
    printf("[dbg] timer_init: hart=%d, mtime=%p, mtimecmp=%p, interval=%lu\n",
           hartid, mtime, mtimecmp, INTERVAL);
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
    uint64 now_total = sys_total_timer.total_ticks;
    spinlock_release(&sys_total_timer.lk);

    // bump per-cpu ticks
    spinlock_acquire(&sys_timer[cpuid].lk);
    sys_timer[cpuid].ticks++;
    uint64 hart_ticks = sys_timer[cpuid].ticks;
    spinlock_release(&sys_timer[cpuid].lk);

    // *** DEBUG PRINT so we SEE timer interrupts actually happening ***
    // this should show up even while user code is running
    printf("TIMER TICK: cpu=%d hart_ticks=%lu total=%lu\n",
           cpuid, hart_ticks, now_total);

    // clear SSIP (software interrupt pending bit for S-mode, bit1 of sip)
    // this acknowledges the "timer interrupt" we synthesized in timer_vector
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
