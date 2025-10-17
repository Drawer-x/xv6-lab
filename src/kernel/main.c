#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"

volatile int cpu0_ready = 0;
volatile int cpu0_first_dida = 0;     

int main() {
    int cpuid = r_tp();

    if (cpuid == 0) {
        printf("cpu %d is booting!\n", cpuid);
        trap_kernel_init(); 
        timer_update(); 
        cpu0_first_dida = 1; 
        __sync_synchronize();
        cpu0_ready = 1;
    } else {
        while (cpu0_ready == 0) {
            asm volatile("nop");
        }
        while (cpu0_first_dida == 0) {
            asm volatile("nop");
        }
    }

    trap_kernel_inithart();
    
    if (cpuid == 1) {
        printf("cpu %d is booting!\n", cpuid);
        timer_update();  // 输出CPU1的di da
    }
    // uint64 sstatus = r_sstatus();
    // sstatus |= SSTATUS_SIE;  // 最后开启中断总开关
    // w_sstatus(sstatus);

    while (1) {
        asm volatile("wfi");
    }
    return 0;
}
