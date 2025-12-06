#include "mod.h"
#include "../lib/method.h"
#include "../fs/type.h"   // VIRTIO_IRQ

// PLIC初始化：设置优先级
void plic_init()
{
    // UART
    *(uint32 *)(PLIC_PRIORITY(UART_IRQ)) = 1;
    // **新增**：VIRTIO 磁盘
    *(uint32 *)(PLIC_PRIORITY(VIRTIO_IRQ)) = 1;
}

// PLIC核心初始化：打开 S 模式可见的中断线
void plic_inithart()
{
    int hartid = mycpuid();

    // 使能外部中断开关（S态）：UART + VIRTIO
    *(uint32 *)PLIC_SENABLE(hartid) = (1 << UART_IRQ) | (1 << VIRTIO_IRQ);

    // 设置 S 态中断优先级阈值：允许所有优先级 >=1 的中断进入
    *(uint32 *)PLIC_SPRIORITY(hartid) = 0;
}

// 获取中断号
int plic_claim(void)
{
    int hartid = mycpuid();
    int irq = *(uint32 *)PLIC_SCLAIM(hartid);
    return irq;
}

// 确认该中断号对应中断已经完成
void plic_complete(int irq)
{
    int hartid = mycpuid();
    *(uint32 *)PLIC_SCLAIM(hartid) = irq;
}
