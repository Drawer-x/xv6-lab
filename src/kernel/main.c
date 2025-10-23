#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"
#include "proc/mod.h"

volatile static int started = 0;

int main()
{
    // —— 诊断：main 是否被执行？（直接 MMIO 写 UART）——
    *(volatile unsigned char *)(UART_BASE) = 'M';  // 期望先看到一个 'M'

    int cpuid = r_tp();

    if (cpuid == 0) {
        print_init();
        printf("cpu %d is booting!\n", cpuid);

        pmem_init();
        kvm_init();
        kvm_inithart();
        trap_kernel_init();
        trap_kernel_inithart();

        // ✅ 创建第一个用户进程，并直接 swtch 进入（在 proc_make_first() 内完成）
        printf("[main] calling proc_make_first()...\n");
        proc_make_first();
        printf("[main] returned from proc_make_first()\n");


        __sync_synchronize();
        started = 1;

        panic("unreachable: CPU0 should have switched to user mode");
    } else {
        while (started == 0)
            ;
        __sync_synchronize();

        printf("cpu %d is booting!\n", cpuid);

        kvm_inithart();
        trap_kernel_inithart();

        // 其他核暂不参与用户进程调度，可直接 idle
        for (;;) asm volatile("wfi");
    }

    return 0;
}
