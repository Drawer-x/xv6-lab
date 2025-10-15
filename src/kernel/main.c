// main.c - 清理后的版本
#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

volatile int cpu0_ready = 0;

int main()
{
    int cpuid = r_tp();
    printf("CPU%d: booting\n", cpuid);
    timer_init();  // 移到这里，确保所有核心启动后都初始化定时器
    if (cpuid == 0) {
        printf("=== Starting OS initialization ===\n");
        trap_kernel_init();
        printf("OS started\n");
        cpu0_ready = 1;
    } else {
        while (cpu0_ready == 0) {
            asm volatile("nop");
        }
    }

    trap_kernel_inithart();
    printf("CPU%d: ready\n", cpuid);

    while (1) {
        asm volatile("wfi");
    }
    return 0;
}