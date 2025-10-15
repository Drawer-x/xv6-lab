#include "../arch/mod.h"       // 包含 RISC-V 寄存器操作（r_mhartid/w_satp 等）
#include "../trap/method.h"    // 包含 timer_init() 函数声明（时钟初始化需用）
#include "../trap/mod.h"       // 包含中断委托、mstatus 相关常量定义（避免魔数）

// 每个CPU的初始栈（16字节对齐，满足RISC-V函数调用规范）
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

// 内核S-mode入口函数（声明，来自main.c）
extern void main();

void start()
{
    // 1. 暂时不开启分页，使用物理地址（切换到S-mode前避免分页配置冲突）
    w_satp(0);

    // 2. 保存当前CPU核心ID（hartid）到tp寄存器（后续mycpuid()通过tp读取）
    int id = r_mhartid();
    w_tp(id);

    // 3. 委托S-mode处理所有trap（中断+异常，减少M-mode干预）
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
    // 作用：配置当前CPU的MTIMECMP（定时器比较值），启用M-mode时钟中断
    timer_init();

    // 5. 修改mstatus寄存器：伪装上一个状态是S-mode（mret后跳转到S-mode）
    uint64 status = r_mstatus();
    status &= ~MSTATUS_MPP_MASK;  // 清除原有特权级（MPP：Previous Privilege Mode）
    status |= MSTATUS_MPP_S;      // 设置上一个特权级为S-mode
    status |= MSTATUS_MIE; //1.3
    w_mstatus(status);

    // 6. 设置M-mode的返回地址（mepc）：指向S-mode的入口函数main()
    w_mepc((uint64)main);

    // 7. 触发状态迁移：从M-mode跳转到S-mode（执行mepc指向的main()）
    asm volatile("mret");  // 关键指令：切换特权级+跳转到mepc地址

    // 理论上不会执行到这里（mret已跳转），若到这里说明切换失败，进入休眠
    while (1) {
        asm volatile("wfi");  // 无中断时CPU休眠，降低空转消耗
    }
}