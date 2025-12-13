#include "../arch/mod.h"
#include "../trap/method.h"
#include "../trap/mod.h"

// 每个CPU在运行操作系统时需要一个初始的函数栈
__attribute__((aligned(16))) uint8 CPU_stack[4096 * NCPU];

extern void main();

// 允许 S/U 访问全部物理内存，避免默认 PMP 封锁 S 态取指/访存
static inline void pmp_open_all()
{
    asm volatile("li t0, -1\ncsrw pmpaddr0, t0\nli t0, 0x1f\ncsrw pmpcfg0, t0\n" ::
                     : "t0");
}

void start()
{
    // 1. 暂时不开启分页，使用物理地址
    w_satp(0);

    // 2. 保存当前CPU核心ID（hartid）到tp寄存器
    int id = r_mhartid();
    w_tp(id);

    // 3. 放宽 PMP，允许 S/U 访问物理内存
    pmp_open_all();

    // 4. 委托S-mode处理所有trap
    uint64 mideleg = 0;
    mideleg |= (1 << 1);   // SSIP  (S-mode software interrupt)
    mideleg |= (1 << 5);   // STIP  (S-mode timer interrupt)
    mideleg |= (1 << 9);   // SEIP  (S-mode external interrupt)
    w_mideleg(mideleg);

    uint64 medeleg = r_medeleg();
    medeleg |= 0xFFFF;     // 委托前16种异常
    w_medeleg(medeleg);

    // 5. 初始化时钟中断，并设置 mtvec
    timer_init();

    // 6. 允许 S 态访问计数器/时间寄存器
    w_mcounteren(~0ULL);

    // 7. 准备从 M 模式降到 S 模式执行 main()
    uint64 status = r_mstatus();
    status &= ~MSTATUS_MPP_MASK;
    status |= MSTATUS_MPP_S;  // 下一模式 S
    status &= ~MSTATUS_MIE;   // 关闭 MIE，防止 mret 前触发
    status |= (1L << 7);      // 设置 MPIE (bit7)，mret 后 S 态可开中断
    w_mstatus(status);

    // 8. 设置M-mode返回地址为 main()，这样mret后会跳到S态跑main
    w_mepc((uint64)main);

    // 9. 从M态切到S态，进入main()
    asm volatile("mret");

    // 按理说永远到不了这里
    while (1) {
        asm volatile("wfi");
    }
}
