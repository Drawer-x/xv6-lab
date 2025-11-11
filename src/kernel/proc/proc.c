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
#define PTE2PA(pte) (((pte) >> 10) << 12)
// ========== 本文件仅使用你仓库里已有的常量/函数/结构 ==========

// 一些CSR小工具（只在本文件内部使用）

// ---- 进程0的用户空间布局（按实验约定可在此处配置）----
#ifndef USER_BASE
#define USER_BASE 0x0   // 用户代码基地址
#endif
#ifndef PROC0_USTACK_PAGES
#define PROC0_USTACK_PAGES 1               // 先给1页用户栈
#endif
#ifndef PROC0_USTACK_TOP
#define PROC0_USTACK_TOP  (USER_BASE + 8*PGSIZE) // 预留一段空隙，便于观察
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

pgtbl_t proc_pgtbl_init(uint64 trapframe_pa)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: root alloc failed");
    memset(pgtbl, 0, PGSIZE);

    // 映射 trampoline 到 TRAMPOLINE 虚拟地址（内核权限）
    uint64 tramp_pa = (uint64)ALIGN_DOWN((uint64)trampoline, PGSIZE);
    uint64 tramp_va = TRAMPOLINE;
    
    uart_puts("[proc_pgtbl_init] mapping trampoline: VA=");
    uart_puthex(tramp_va);
    uart_puts(" -> PA=");
    uart_puthex(tramp_pa);
    uart_puts("\n");
    
    vm_mappages(pgtbl, tramp_va, tramp_pa, PGSIZE, PTE_R | PTE_X); // 移除PTE_U，仅内核访问
    
    // 映射 trapframe 到 TRAPFRAME 虚拟地址（内核权限）
    vm_mappages(pgtbl, TRAPFRAME, trapframe_pa, PGSIZE, PTE_R | PTE_W); // 移除PTE_U，仅内核访问
    
    return pgtbl;
}

/*
 * 创建并切入第一个用户进程 proczero：
 *  - 分配 trapframe（内核页）
 *  - 建用户页表；映射：trampoline、initcode（RXU）、ustack（RWU）
 *  - 准备 trapframe 的"返回内核"字段：kernel satp、kstack、trap向量、tp
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

    // 2) 用户页表 - 现在传入trapframe物理地址用于映射
    proczero.pgtbl = proc_pgtbl_init((uint64)proczero.tf);

// 3) 装载 initcode 到用户地址空间（1页）
assert(target_user_initcode_len <= PGSIZE, "initcode too large (>1 page)");
void *ucode_page = pmem_alloc(false);
assert(ucode_page != NULL, "proc_make_first: ucode alloc failed");

uart_puts("[DEBUG] ucode_page allocated at: ");
uart_puthex((uint64)ucode_page);
uart_puts("\n");

// 清空并复制 initcode
memset(ucode_page, 0, PGSIZE);
memmove(ucode_page, target_user_initcode, (uint32)target_user_initcode_len);

// 简单的字节检查 - 使用 char 类型
uart_puts("[DEBUG] First 8 bytes at ucode_page: ");
char *bytes = (char*)ucode_page;
for (int i = 0; i < 8; i++) {
    uart_puthex(bytes[i] & 0xFF);
    uart_puts(" ");
}
uart_puts("\n");

// 检查原始 initcode 的前8个字节
uart_puts("[DEBUG] First 8 bytes of target_user_initcode: ");
char *src_bytes = (char*)target_user_initcode;
for (int i = 0; i < 8; i++) {
    uart_puthex(src_bytes[i] & 0xFF);
    uart_puts(" ");
}
uart_puts("\n");

// 验证复制是否正确
int copy_ok = 1;
for (int i = 0; i < 8; i++) {
    if (bytes[i] != src_bytes[i]) {
        copy_ok = 0;
        break;
    }
}
if (copy_ok) {
    uart_puts("[DEBUG] Copy verification: PASS\n");
} else {
    uart_puts("[DEBUG] Copy verification: FAIL\n");
}

// 检查第一个指令 - 使用 unsigned int 代替 uint32_t
unsigned int first_instr = *(unsigned int*)ucode_page;
uart_puts("[DEBUG] First instruction in ucode_page: ");
uart_puthex(first_instr);
uart_puts("\n");

// 映射到用户空间
vm_mappages(proczero.pgtbl, USER_BASE, (uint64)ucode_page, PGSIZE, PTE_R | PTE_X | PTE_U);
uart_puts("[DEBUG] Mapped USER_BASE to physical page\n");
// 4) 分配并映射用户栈（向上生长，sp 初值为 TOP）
uint64 ustack_bot = ALIGN_DOWN(PROC0_USTACK_TOP - PROC0_USTACK_PAGES * PGSIZE, PGSIZE);
void *first_ustack_pa = NULL;  // 保存第一个页面用于调试

for (uint64 i = 0; i < PROC0_USTACK_PAGES; i++) {
    void *pg = pmem_alloc(false);
    assert(pg != NULL, "proc_make_first: ustack alloc failed");
    vm_mappages(proczero.pgtbl, ustack_bot + i * PGSIZE, (uint64)pg, PGSIZE, PTE_R | PTE_W | PTE_U);
    
    if (i == 0) {
        first_ustack_pa = pg;  // 保存第一个页面地址
    }
}

proczero.ustack_npage = PROC0_USTACK_PAGES;
proczero.heap_top = USER_BASE + PGSIZE; // 堆顶在用户代码页之后

// 调试输出
uart_puts("[DEBUG] User stack: VA=[");
uart_puthex(ustack_bot);
uart_puts(", ");
uart_puthex(ustack_bot + PROC0_USTACK_PAGES * PGSIZE);
uart_puts("), first PA=");
uart_puthex((uint64)first_ustack_pa);
uart_puts("\n");

    // 5) 进程的"内核栈"和 context
    proczero.kstack = alloc_kstack_page();
    memset(&proczero.ctx, 0, sizeof(proczero.ctx));
    proczero.ctx.ra = (uint64)trap_user_return;     // swtch 进入后，先走 trap_user_return
    proczero.ctx.sp = proczero.kstack + PGSIZE;     // 内核栈顶

    // 6) 预置 trapframe 的"回内核"必需字段
    proczero.tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);
    proczero.tf->user_to_kern_sp = proczero.kstack + PGSIZE;
    proczero.tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    proczero.tf->user_to_kern_hartid = r_tp();

    // 7) 预置"首次进入用户态"的寄存器
    proczero.tf->user_to_kern_epc = USER_BASE; // 从 USER_BASE 开始执行
    proczero.tf->sp = PROC0_USTACK_TOP;

// 8) 切换到 proczero
cpu_t *c = mycpu();
c->proc = &proczero;

// === 修改后的调试代码 ===
uart_puts("[DEBUG] Pre-switch state check:\n");
uart_puts("  User SATP value: ");
uart_puthex(MAKE_SATP(proczero.pgtbl));
uart_puts("\n");

uart_puts("  Kernel SATP value: ");
uart_puthex(r_satp());
uart_puts("\n");

uart_puts("  User context RA: ");
uart_puthex(proczero.ctx.ra);
uart_puts("\n");

uart_puts("  User context SP: ");
uart_puthex(proczero.ctx.sp);
uart_puts("\n");

// 只检查物理地址，不检查虚拟地址 0x1000
uart_puts("[DEBUG] Checking physical address content directly:\n");
uart_puts("  Known physical address: ");
uart_puthex((uint64)ucode_page);
uart_puts("\n");
unsigned int *pa_instr = (unsigned int*)ucode_page;
uart_puts("  Instruction at physical address: ");
uart_puthex(pa_instr[0]);
uart_puts("\n");


// === 调试代码结束 ===

uart_puts("[proc_make_first] switching to user mode\n");
swtch(&c->ctx, &proczero.ctx);
}