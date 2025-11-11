#include "mod.h"
#include "method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../lib/method.h"
#include "../lib/type.h"
#include "../../user/initcode.h"
#include "../trap/method.h"
extern pgtbl_t kernel_pgtbl;


// trampoline & user-vector（在汇编里）
extern char trampoline[];
extern char user_vector[];
// 汇编出口：从内核返回用户态
extern void user_return(trapframe_t *tf, uint64 satp);

// ========== 本文件仅使用你仓库里已有的常量/函数/结构 ==========

// 一些CSR小工具（只在本文件内部使用）

// ---- 进程0的用户空间布局（按实验约定可在此处配置）----
// 若你的 README/脚本对用户地址另有明确要求，改成相应值即可。
#ifndef PROC0_UCODE_VA       // 先判断宏是否未定义
#define PROC0_UCODE_VA 0x0   // 单独定义宏的值为 0x0
#endif
#ifndef PROC0_USTACK_PAGES
#define PROC0_USTACK_PAGES 1               // 先给1页用户栈
#endif
#ifndef PROC0_USTACK_TOP
#define PROC0_USTACK_TOP  (PROC0_UCODE_VA + 8*PGSIZE) // 预留一段空隙，便于观察
#endif

// 内核全局的第一个进程（仅 lab-4）
static proc_t proczero;

// 为进程分配一个内核栈（直接用可分配物理页，恒等映射即可）
static uint64 alloc_kstack_page()
{
    void *page = pmem_alloc(true);
    assert(page != NULL, "alloc_kstack_page: pmem_alloc failed");
    memset(page, 0, PGSIZE);
    return (uint64)page; // 恒等映射：VA == PA（见 kvm.c 的 ALLOC 区恒等映射）
}

/*
 * 为“用户页表”做最小初始化：
 *  - 分配根页表
 *  - 在“同一虚拟地址”上映射 trampoline（一页，RX）
 *    这样切到用户页表后，trampoline 仍可在相同 VA 上被命中
 */
// pgtbl_t proc_pgtbl_init(uint64 /*trapframe_pa: 未在本阶段映射到用户VA*/)
// {
//     pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
//     assert(pgtbl != NULL, "proc_pgtbl_init: root alloc failed");
//     memset(pgtbl, 0, PGSIZE);

//     // trampoline 同VA映射（与内核一致）
//     uint64 tramp_va = (uint64)ALIGN_DOWN((uint64)trampoline, PGSIZE);
//     uint64 tramp_pa = tramp_va; // 你的内核页表是恒等映射，trampoline 也在内核地址段
//     vm_mappages(pgtbl, tramp_va, tramp_pa, PGSIZE, PTE_R | PTE_X| PTE_U); // 不标U位
// // 在 proc_pgtbl_init 中添加检查
// printf("[proc] trampoline mapped at VA 0x%lx -> PA 0x%lx\n", 
//        tramp_va, tramp_pa);
//     return pgtbl;
// }
pgtbl_t proc_pgtbl_init(uint64 /*trapframe_pa: 未在本阶段映射到用户VA*/)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: root alloc failed");
    memset(pgtbl, 0, PGSIZE);

    // 映射 trampoline 到 TRAMPOLINE 虚拟地址
    uint64 tramp_pa = (uint64)ALIGN_DOWN((uint64)trampoline, PGSIZE);
    uint64 tramp_va = TRAMPOLINE;  // 使用 TRAMPOLINE 常量作为虚拟地址
    
    uart_puts("[proc_pgtbl_init] mapping trampoline: VA=");
    uart_puthex(tramp_va);
    uart_puts(" -> PA=");
    uart_puthex(tramp_pa);
    uart_puts("\n");
    
    vm_mappages(pgtbl, tramp_va, tramp_pa, PGSIZE, PTE_R | PTE_X | PTE_U);
    
    return pgtbl;
}

/*
 * 创建并切入第一个用户进程 proczero：
 *  - 分配 trapframe（内核页）
 *  - 建用户页表；映射：trampoline、initcode（RXU）、ustack（RWU）
 *  - 准备 trapframe 的“返回内核”字段：kernel satp、kstack、trap向量、tp
 *  - 准备 context：ra=trap_user_return，sp=内核栈顶
 *  - 把当前CPU的 proc 指向 proczero，并 swtch 进去
 */
void proc_make_first()
{
    // 清零 & 基本标识
    memset(&proczero, 0, sizeof(proczero));
    proczero.pid = 0;

    // 1) trapframe（仅在内核访问，放内核物理页）
    proczero.tf = (trapframe_t *)pmem_alloc(true);
    assert(proczero.tf != NULL, "proc_make_first: trapframe alloc failed");
    memset(proczero.tf, 0, PGSIZE);

    // 2) 用户页表
    proczero.pgtbl = proc_pgtbl_init((uint64)proczero.tf);

    // 3) 装载 initcode 到用户地址空间（1页）
    assert(target_user_initcode_len <= PGSIZE, "initcode too large (>1 page)");
    void *ucode_page = pmem_alloc(false);
    assert(ucode_page != NULL, "proc_make_first: ucode alloc failed");
    memset(ucode_page, 0, PGSIZE);
    memmove(ucode_page, target_user_initcode, (uint32)target_user_initcode_len);
    vm_mappages(proczero.pgtbl, 0x0, (uint64)ucode_page, PGSIZE, PTE_R | PTE_X | PTE_U);

    // 4) 分配并映射用户栈（向上生长，sp 初值为 TOP）
    uint64 ustack_bot = ALIGN_DOWN(PROC0_USTACK_TOP - PROC0_USTACK_PAGES * PGSIZE, PGSIZE);
    for (uint64 i = 0; i < PROC0_USTACK_PAGES; i++) {
        void *pg = pmem_alloc(false);
        assert(pg != NULL, "proc_make_first: ustack alloc failed");
        vm_mappages(proczero.pgtbl, ustack_bot + i * PGSIZE, (uint64)pg, PGSIZE, PTE_R | PTE_W | PTE_U);
    }
    proczero.ustack_npage = PROC0_USTACK_PAGES;
    proczero.heap_top     = ustack_bot; // 简单约定：堆顶先放在栈底之下
// 检查用户栈映射
printf("[proc] ustack mapped at VA [0x%lx, 0x%lx]\n", 
       ustack_bot, ustack_bot + PROC0_USTACK_PAGES * PGSIZE);
    // 5) 进程的“内核栈”和 context
    proczero.kstack = alloc_kstack_page();
    memset(&proczero.ctx, 0, sizeof(proczero.ctx));
    proczero.ctx.ra = (uint64)trap_user_return;     // swtch 进入后，先走 trap_user_return
    proczero.ctx.sp = proczero.kstack + PGSIZE;     // 内核栈顶

    // 6) 预置 trapframe 的“回内核”必需字段（供 user->kernel 的 user_vector 使用）
    //    对照 trampoline.S：进入 user_vector 后会：
    //      sp <- user_to_kern_sp
    //      tp <- user_to_kern_hartid
    //      t0 <- user_to_kern_trapvector （即 C 端 trap_user_handler 的地址）
    //      satp <- user_to_kern_satp （切回内核页表）
    proczero.tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);
    proczero.tf->user_to_kern_sp         = proczero.kstack + PGSIZE; // 进内核用的栈顶
    proczero.tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    proczero.tf->user_to_kern_hartid     = r_tp();                   // “保存内核 tp”（你的注释就是这么要求的）

    // 7) 预置“首次进入用户态”的寄存器
    //    trampoline 的 user_return 会把 tf 中通用寄存器恢复后 sret
    proczero.tf->user_to_kern_epc = 0x0; // 作为“初次 sepc”
    proczero.tf->sp               = PROC0_USTACK_TOP;

     // map trapframe page into user pagetable (S-only access)
    vm_mappages(
        proczero.pgtbl,
        (uint64)proczero.tf,
        (uint64)proczero.tf,
        PGSIZE,
        PTE_R | PTE_W
    );

    // map kernel stack page into user pagetable (S-only access)
    vm_mappages(
        proczero.pgtbl,
        (uint64)proczero.kstack,
        (uint64)proczero.kstack,
        PGSIZE,
        PTE_R | PTE_W
    );

    // 8) 切换到 proczero
    cpu_t *c = mycpu();
    c->proc = &proczero;

// 替换printf为UART打印
uart_puts("[proc_make_first] initcode va=");
uart_puthex(PROC0_UCODE_VA);
uart_puts(", pa=");
uart_puthex((uint64)ucode_page);
uart_puts("\n");

uart_puts("[proc_make_first] tf->user_to_kern_epc=");
uart_puthex(proczero.tf->user_to_kern_epc);
uart_puts("\n");

uart_puts("[proc_make_first] tf->sp (usp)=");
uart_puthex(proczero.tf->sp);
uart_puts("\n");
// // 检查 0x0 对应的页表项
// pte_t *pte = walk_pgtbl(proczero.pgtbl, PROC0_UCODE_VA, 0);
// assert(pte != NULL, "[proc_make_first] 0x0 pte not found!");
// uint64 pte_flags = *pte & (PTE_V | PTE_R | PTE_W | PTE_X | PTE_U);
// printf("[proc_make_first] 0x0 pte flags: V=%d, R=%d, W=%d, X=%d, U=%d\n",
//        (pte_flags & PTE_V) ? 1 : 0,  // 必须为 1（有效）
//        (pte_flags & PTE_R) ? 1 : 0,
//        (pte_flags & PTE_W) ? 1 : 0,
//        (pte_flags & PTE_X) ? 1 : 0,  // 必须为 1（可执行）
//        (pte_flags & PTE_U) ? 1 : 0); // 必须为 1（用户态可访问）
// 在 proc_make_first() 末尾添加简化的调试输出：
uart_puts("[proc_make_first] === Simplified Debug ===\n");

uart_puts("initcode PA: ");
uart_puthex((uint64)ucode_page);
uart_puts("\n");

uart_puts("ustack VA: ");
uart_puthex((uint64)ustack_bot);
uart_puts(" to ");
uart_puthex(ustack_bot + PROC0_USTACK_PAGES * PGSIZE);
uart_puts("\n");

uart_puts("trampoline VA: ");
uart_puthex((uint64)ALIGN_DOWN((uint64)trampoline, PGSIZE));
uart_puts("\n");

uart_puts("trapframe: ");
uart_puthex((uint64)proczero.tf);
uart_puts("\n");

uart_puts("user epc: ");
uart_puthex(proczero.tf->user_to_kern_epc);
uart_puts("\n");

uart_puts("user sp: ");
uart_puthex(proczero.tf->sp);
uart_puts("\n");

// 检查关键地址是否有效
uart_puts("Checking if 0x0 is mapped...\n");
// 这里可以添加一个简单的测试，比如尝试读取 0x0 地址的内容
// 但要注意这可能会触发页错误

uart_puts("[proc_make_first] switching to user mode\n");
    swtch(&c->ctx, &proczero.ctx);

    // swtch 返回到这里时，说明 proczero 让出了CPU（本实验阶段通常不会回来）
}