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