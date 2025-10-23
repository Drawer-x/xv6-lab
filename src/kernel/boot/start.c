#include "../arch/mod.h"      
#include "../trap/method.h"   
#include "../trap/mod.h"    

// 每个CPU在运行操作系统时需要一个初始的函数栈
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

extern void main();

void start()
{
    // 探针1：刚进入 start()
    *(volatile unsigned char*)UART_BASE = 'S';

    // 1. 暂时不开启分页，使用物理地址
    w_satp(0);

    // 2. 保存当前CPU核心ID（hartid）到tp寄存器
    int id = r_mhartid();
    w_tp(id);

    // 3. 委托S-mode处理所有trap
    // 3.1 中断委托（mideleg）：将S-mode应处理的中断委托给S-mode
    uint64 mideleg = 0;
    mideleg |= (1 << 1);   // 软件中断
    mideleg |= (1 << 5);   // 时钟中断  
    mideleg |= (1 << 9);   // 外部中断
    w_mideleg(mideleg);
    // 3.2 异常委托（medeleg）：将所有异常委托给S-mode（简化调试，避免M-mode卡死）
    uint64 medeleg = r_medeleg();
    medeleg |= 0xFFFF;     // 委托所有16类异常（S-mode可通过trap_handler处理）
    w_medeleg(medeleg);

    // 4. 时钟中断初始化（M-mode唯一需处理的中断，每个CPU独立初始化）
    timer_init();

    // 5. 修改mstatus寄存器：伪装上一个状态是S-mode（mret后跳转到S-mode）
    uint64 status = r_mstatus();
    status &= ~MSTATUS_MPP_MASK;  
    status |= MSTATUS_MPP_S;     
    w_mstatus(status);

    // 6. 设置M-mode的返回地址
    w_mepc((uint64)main);

    // 探针2：mret 前
    *(volatile unsigned char*)UART_BASE = 'R';

    // 7. 触发状态迁移：从M-mode跳转到S-mode（执行mepc指向的main()）
    asm volatile("mret");  

    while (1) {
        asm volatile("wfi");  
    }
}