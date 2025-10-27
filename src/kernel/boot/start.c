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
    // 3.1 中断委托（mideleg）：将S-mode级别的中断交给S
    uint64 mideleg = 0;
    mideleg |= (1 << 1);   // SSIP  (S-mode software interrupt)
    mideleg |= (1 << 5);   // STIP  (S-mode timer interrupt)
    mideleg |= (1 << 9);   // SEIP  (S-mode external interrupt)
    w_mideleg(mideleg);

    // 3.2 异常委托（medeleg）：把常见异常也委托给S处理
    uint64 medeleg = r_medeleg();
    medeleg |= 0xFFFF;     // 委托前16种异常
    w_medeleg(medeleg);

    // 4. 初始化时钟中断（M-mode定时器转发为S态软件中断）
    //    这一步会：
    //      - 设置 mtvec = timer_vector
    //      - 设置 mie.MTIE = 1
    //      - 打开 mstatus.MIE = 1
    //      - 安排下一次 mtimecmp
    timer_init();

    // 5. 准备从 M 模式降到 S 模式执行 main()
    //    我们要改 MSTATUS.MPP = S，但一定要保留原本的 MSTATUS_MIE!
    uint64 status = r_mstatus();

    // 把 MPP 清零后设为 S
    status &= ~MSTATUS_MPP_MASK;
    status |= MSTATUS_MPP_S;

    // (关键点) 重新打开 MIE 位，确保 M 态中断仍然允许
    status |= MSTATUS_MIE;

    w_mstatus(status);

    // 6. 设置M-mode返回地址为 main()，这样mret后会跳到S态跑main
    w_mepc((uint64)main);

    // 探针2：mret 前
    *(volatile unsigned char*)UART_BASE = 'R';

    // 7. 从M态切到S态，进入main()
    asm volatile("mret");

    // 按理说永远到不了这里。如果到了，就 idle。
    while (1) {
        asm volatile("wfi");
    }
}
