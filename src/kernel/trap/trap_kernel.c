#include "mod.h"
#include "../lock/type.h"
#include "../lock/method.h"
#include "../arch/method.h"
#include "../lib/mod.h"

// ====================================================
// 内核 Trap Handler：S-mode 层中断与异常处理
// ====================================================

// 中断信息（仅用于报错时打印）
static const char *interrupt_info[16] __attribute__((unused)) = {
    "U-mode software interrupt",      // 0
    "S-mode software interrupt",      // 1
    "reserved-1",                     // 2
    "M-mode software interrupt",      // 3
    "U-mode timer interrupt",         // 4
    "S-mode timer interrupt",         // 5
    "reserved-2",                     // 6
    "M-mode timer interrupt",         // 7
    "U-mode external interrupt",      // 8
    "S-mode external interrupt",      // 9
    "reserved-3",                     // 10
    "M-mode external interrupt",      // 11
    "reserved-4",                     // 12
    "reserved-5",                     // 13
    "reserved-6",                     // 14
    "reserved-7",                     // 15
};

// 异常信息（仅用于报错时打印）
static const char *exception_info[16] __attribute__((unused)) = {
    "Instruction address misaligned", // 0
    "Instruction access fault",       // 1
    "Illegal instruction",            // 2
    "Breakpoint",                     // 3
    "Load address misaligned",        // 4
    "Load access fault",              // 5
    "Store/AMO address misaligned",   // 6
    "Store/AMO access fault",         // 7
    "Environment call from U-mode",   // 8
    "Environment call from S-mode",   // 9
    "reserved-1",                     // 10
    "Environment call from M-mode",   // 11
    "Instruction page fault",         // 12
    "Load page fault",                // 13
    "reserved-2",                     // 14
    "Store/AMO page fault",           // 15
};

// trap.S 中的内核态向量入口
extern void kernel_vector(void);

// 供 handler 使用的外设/时钟中断处理函数
void external_interrupt_handler(void);
void timer_interrupt_handler(void);

// ====================================================
// 初始化
// ====================================================

// 全局初始化（仅一次）
void trap_kernel_init(void)
{
    uart_init();     // 初始化 UART
    plic_init();     // 初始化 PLIC
    timer_create();  // 创建/启动时钟（M 态中断转发为 S 态软件中断 SSIP）
}

// 每个 CPU 的本地初始化
void trap_kernel_inithart(void)
{
    // 初始化当前 hart 的 PLIC 路由
    plic_inithart();

    // 1. 设置 S 态 trap 向量到 kernel_vector
    w_stvec((uint64)kernel_vector);

    // 2. 开启 S 态中断（软件/时钟/外部）
    uint64 sie_val = r_sie();
    sie_val |= (1 << 1);   // SSIE
    sie_val |= (1 << 5);   // STIE
    sie_val |= (1 << 9);   // SEIE
    w_sie(sie_val);

    // 3. 打开全局中断开关 SIE 位
    uint64 sstatus_val = r_sstatus();
    sstatus_val |= SSTATUS_SIE;
    w_sstatus(sstatus_val);

    // 调试输出
    printf("[trap_kernel_inithart] stvec=%p sie=0x%lx sstatus=0x%lx\n",
           kernel_vector, r_sie(), r_sstatus());
}

// ====================================================
// 外设中断处理（来自 PLIC）
// ====================================================
void external_interrupt_handler(void)
{
    int irq = plic_claim();  // 读取当前中断号

    if (irq == UART_IRQ) {
        uart_intr();         // 处理 UART 中断
        plic_complete(irq);  // 必须 complete 否则 IRQ 会一直 pending

    } else if (irq != 0) {
        printf("unexpected external interrupt: irq=%d\n", irq);
        plic_complete(irq);
    }
}

// ====================================================
// 核心的 S 态 trap 分发函数
// ====================================================
void trap_kernel_handler(void)
{
    uint64 sepc    = r_sepc();
    uint64 scause  = r_scause();
    uint64 stval   = r_stval();
    uint64 sstatus __attribute__((unused)) = r_sstatus();

    // 判断是否中断
    uint64 is_intr = (scause >> 63) & 1;
    uint64 code    = scause & 0xfff;

    if (is_intr) {
        // ----------------------
        // 异步中断 (interrupt)
        // ----------------------
        switch (code) {
        case 1:  // SSIP: M 态 timer 向 S 态发的“软件中断”
        case 5:  // STIP: S 态时钟中断
            timer_interrupt_handler();
            break;

        case 9:  // SEIP: 外部中断（PLIC）
            external_interrupt_handler();
            break;

        default:
            printf("[trap_kernel_handler] unexpected interrupt code=%lu\n", code);
            printf("sepc=%p stval=%p scause=%lx\n",
                   (void *)sepc, (void *)stval, scause);
            break;
        }

    } else {
        // ----------------------
        // 同步异常 (exception)
        // ----------------------
        switch (code) {
        case 8:  // ecall from U-mode
            printf("[S-trap] ecall from user @ sepc=%p\n", (void *)sepc);
            w_sepc(sepc + 4);  // 跳过 ecall
            break;

        case 9:  // ecall from S-mode
            printf("[S-trap] ecall from supervisor @ sepc=%p\n", (void *)sepc);
            w_sepc(sepc + 4);
            break;

        default:
            printf("[trap_kernel_handler] unexpected exception code=%lu\n", code);
            printf("sepc=%p stval=%p scause=%lx\n",
                   (void *)sepc, (void *)stval, scause);
            panic("unhandled trap in S-mode");
        }
    }
}
