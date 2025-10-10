#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h" // 假设包含时钟、中断相关声明

// 全局变量，标记各 CPU 是否已完成启动打印
int cpu_booted[2] = {0, 0}; 

int main()
{
    int cpuid = r_tp(); // 获取当前 CPU 核心 ID

    // 1. CPU 启动提示（每个 CPU 仅在首次执行时打印）
    if (cpuid < 2) { // 假设最多 2 个 CPU 核心
        if (cpu_booted[cpuid] == 0) {
            printf("cpu %d is booting!\n", cpuid);
            cpu_booted[cpuid] = 1;
        }
    }

    // 2. 仅 CPU-0 初始化全局资源（避免多核心重复初始化）
    if (cpuid == 0) {
        trap_kernel_init(); // 初始化全局资源：PLIC、UART、全局时钟
        printf("OS started on CPU %d\n", cpuid);
        printf("Initial system ticks: %lld (1 tick ≈ 0.1s)\n", timer_get_ticks());
    }

    // 3. 每个核心初始化私有中断资源（所有核心必须执行）
    trap_kernel_inithart(); // 设置中断向量、使能 S-mode 中断

    // 4. 死循环：等待中断（CPU 休眠，降低功耗）
    while (1) {
        asm volatile("wfi"); // Wait For Interrupt：无中断时 CPU 进入休眠
    }
    return 0;
}