// main.c - 清理后的版本
#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

volatile int cpu0_ready = 0;

int main() {
    int cpuid = r_tp();
    printf("cpu %d is booting!\n", cpuid);

    if (cpuid == 0) {
        printf("CPU0: 开始执行OS初始化（trap_kernel_init）\n");
        trap_kernel_init();
        printf("CPU0: OS初始化完成，设置cpu0_ready=1\n");
        __sync_synchronize();
        cpu0_ready = 1;
    } else {
        printf("CPU1: 等待CPU0完成初始化（当前cpu0_ready=%d）\n", cpu0_ready);
        while (cpu0_ready == 0) {
            asm volatile("nop");
        }
        printf("CPU1: 检测到CPU0就绪，继续执行\n");
    }

    timer_create();
    trap_kernel_inithart();
    uint64 stvec_val = r_stvec();
    printf("[DEBUG] CPU%d: stvec=%p\n", r_tp(), (void*)stvec_val);
    printf("CPU%d: 所有初始化完成，进入wfi循环\n", cpuid);

    while (1) {
        asm volatile("wfi");
    }
    return 0;
}
