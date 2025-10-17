// main.c - 清理后的版本
#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

volatile int cpu0_ready = 0;
volatile int cpu0_first_dida = 0;     // 新增：CPU0已输出首次di da

int main() {
    int cpuid = r_tp();
    //printf("cpu %d is booting!\n", cpuid);

    if (cpuid == 0) {
        printf("cpu %d is booting!\n", cpuid);
        // CPU0 流程：启动 → 初始化 → 首次di da → 允许CPU1启动
        //printf("CPU0: 开始执行OS初始化（trap_kernel_init）\n");
        trap_kernel_init();  // 初始化外设和时钟（含timer_create）
        
        // 主动触发CPU0的首次di da（在CPU1启动前）
        //printf("CPU0: 触发首次di da\n");
        timer_update();  // 直接调用一次，输出CPU0的di da
        cpu0_first_dida = 1;  // 标志CPU0首次di da完成
        
        // 通知CPU0初始化完成
        //printf("CPU0: OS初始化完成，设置cpu0_ready=1\n");
        __sync_synchronize();
        cpu0_ready = 1;
        // 启动完成后提示 UART 测试
        printf("\n=== UART 输入测试开始 ===\n");
        printf("请输入字符（支持 Backspace 和 Enter），输入 'q' 退出测试\n");
    } else {
        // CPU1 流程：等待CPU0初始化 → 等待CPU0首次di da → 自身启动 → 首次di da
        //printf("CPU1: 等待CPU0完成初始化（当前cpu0_ready=%d）\n", cpu0_ready);
        while (cpu0_ready == 0) {
            asm volatile("nop");
        }
        
        // 额外等待CPU0的首次di da完成
        //printf("CPU1: 等待CPU0首次di da（当前cpu0_first_dida=%d）\n", cpu0_first_dida);
        while (cpu0_first_dida == 0) {
            asm volatile("nop");
        }
        printf("cpu %d is booting!\n", cpuid);
        //printf("CPU1: 检测到CPU0就绪且已输出di da，继续执行\n");
    }

    // 执行各自的核心初始化（如中断向量、使能等）
    timer_create();  // 每个CPU初始化自己的时钟（CPU1在此处才执行）
    trap_kernel_inithart();
    
    // CPU1在此时主动触发首次di da（在进入wfi前）
    if (cpuid == 1) {
        //printf("CPU1: 触发首次di da\n");
        timer_update();  // 输出CPU1的di da
    }
    // 所有初始化完成，开启中断
    uint64 sstatus = r_sstatus();
    sstatus |= SSTATUS_SIE;  // 最后开启中断总开关
    w_sstatus(sstatus);
    //printf("CPU%d: 所有初始化完成，进入wfi循环\n", cpuid);

    while (1) {
        asm volatile("wfi");
    }
    return 0;
}
