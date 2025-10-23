#include "mod.h"
#include "../lock/type.h"
#include "../lock/method.h"
#include "../arch/method.h"
#include "../lib/mod.h"

// 中断信息（仅用于报错时打印）
static const char *interrupt_info[16] = {
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

static const char *exception_info[16] = {
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

// 共享初始化（一次性）
void trap_kernel_init(void)
{
    uart_init();     // 早期串口可用
    plic_init();     // PLIC 全局初始化
    timer_create();  // 创建/启动时钟（M 态中断转发为 SSIP）
}

// 每核初始化（本核独有）
void trap_kernel_inithart(void)
{
    // PLIC 路由到本核
    plic_inithart();

    // 将 S 态 trap 入口设为内核态向量（kernel_vector）
    // stvec[1:0] = 0（direct 模式），我们这里不设置 vectored
    w_stvec((uint64)kernel_vector);

    // 开启 S 态全局中断位
    uint64 sstatus = r_sstatus();
    sstatus |= SSTATUS_SIE;
    w_sstatus(sstatus);

    // 开启定时器/外部/软件中断
    uint64 sie_val = r_sie();
    sie_val |= SIE_STIE;  // S-mode timer interrupt enable
    sie_val |= SIE_SEIE;  // S-mode external interrupt enable (PLIC)
    sie_val |= SIE_SSIE;  // S-mode software interrupt enable (M->S 转发)
    w_sie(sie_val);

    intr_on();
}

// 由 kernel_vector() 进入
void trap_kernel_handler(void)
{
    uint64 sepc    = r_sepc();     // 触发时的 PC
    uint64 sstatus = r_sstatus();  // SPP/SIE 等
    uint64 scause  = r_scause();   // 原因
    uint64 stval   = r_stval();    // 附加信息

    // 来自 S 模式，且进入 handler 时应为关中断状态（避免嵌套）
    assert(sstatus & SSTATUS_SPP, "trap_kernel_handler: not from s-mode");
    assert(intr_get() == 0, "trap_kernel_handler: interrupt enabled");

    uint64 is_intr = (scause >> 63) & 1;
    uint64 code    = scause & 0xfff;   // 原因码统一取低 12 位

    if (is_intr) {
        // ===== 中断 =====
        switch (code) {
        case 1: // SSIP：M 态时钟通过置 SSIP 通知 S 态
        case 5: // STIP：S 态时钟中断
            timer_interrupt_handler(); // 内部会清除 SSIP（见 trap_kernel.c 底部）
            break;
        case 9: // SEIP：外部中断（PLIC），其中可能是 UART
            external_interrupt_handler();
            break;
        default:
            printf("\nunexpected interrupt: %s\n", (code < 16 ? interrupt_info[code] : "unknown"));
            printf("code = %lu, sepc = %p, stval = %p\n", code, sepc, stval);
            panic("trap_kernel_handler");
        }
    } else {
        // ===== 异常 =====
        switch (code) {
        case 0: // Instruction address misaligned
        case 1: // Instruction access fault
            printf("\n[EXCEPTION] %s\n", exception_info[code]);
            printf("  sepc: %p\n", sepc);
            printf("  stval: %p\n", stval);
            panic("instruction access fault");

        case 2: // Illegal instruction
            printf("\n[EXCEPTION] %s\n", exception_info[code]);
            printf("  sepc: %p\n", sepc);
            printf("  instruction at sepc: 0x%lx\n", *(uint64*)sepc);
            panic("illegal instruction");

        case 5: // Load access fault
        case 7: // Store/AMO access fault
            printf("\n[EXCEPTION] %s\n", exception_info[code]);
            printf("  sepc: %p\n", sepc);
            printf("  stval: %p\n", stval);
            panic("memory access fault");

        case 8: // Environment call from U-mode（一般不期望在内核态看到）
            printf("[SYSCALL] %s at %p\n", exception_info[code], sepc);
            w_sepc(sepc + 4);
            break;

        case 9: // Environment call from S-mode
            printf("[S-MODE ECALL] %s at %p\n", exception_info[code], sepc);
            w_sepc(sepc + 4);
            break;

        case 12: // Instruction page fault
        case 13: // Load page fault
        case 15: // Store/AMO page fault
            printf("[PAGE FAULT] %s at address %p\n", exception_info[code], stval);
            printf("  sepc: %p\n", sepc);
            panic("page fault not implemented");

        default:
            printf("\nunexpected exception: %s\n", (code < 16 ? exception_info[code] : "unknown"));
            printf("code = %lu, sepc = %p, stval = %p\n", code, sepc, stval);
            panic("trap_kernel_handler");
        }
    }
}

// 外设中断处理（基于 PLIC：此处识别 UART）
void external_interrupt_handler(void)
{
    int irq = plic_claim();  // 读取当前中断号
    if (irq == UART_IRQ) {
        uart_intr();
        plic_complete(irq);  // 必须完成，否则 IRQ 会保持 pending
    } else if (irq != 0) {
        // 其他外设中断，先打印再 complete，避免死锁
        printf("unexpected external interrupt: irq=%d\n", irq);
        plic_complete(irq);
    }
}

// 时钟中断处理（基于 M 态定时器通过 SSIP 通知 S 态）
// 由 timer_create() 安排周期性中断 -> S 态收到后更新 tick 并清 SSIP
void timer_interrupt_handler(void)
{
    // 只调用一次更新（你的 lab-3 已验证过）
    timer_update();

    // 清除 S 态软件中断 pending 位（SSIP = bit 1）
    // 在 trap.S 里也有对应写 sip 的逻辑；这里确保最终清零
    w_sip(r_sip() & ~2);
}
