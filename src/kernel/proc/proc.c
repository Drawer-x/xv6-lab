#include "mod.h"
#include "method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../lib/method.h"
#include "../lib/type.h"
#include "../../user/initcode.h"

// 外部符号声明（仅保留必需）
extern pgtbl_t kernel_pgtbl;
extern char trampoline[];       // 汇编实现的trampoline
extern char user_vector[];      // 汇编实现的用户态trap入口
extern void user_return(trapframe_t *tf, uint64 satp); // 汇编出口：返回用户态
extern void trap_user_handler();// C实现的用户态trap处理函数
extern void trap_user_return(); // 从内核切回用户态的入口

// 内核全局的第一个进程（仅 lab-4）
static proc_t proczero;
/**
 * 分配内核栈页（使用全局 KSTACK 宏，而非动态分配，对齐系统规范）
 */
static uint64 alloc_kstack_page()
{
    // 进程0的内核栈地址：KSTACK(0)（全局标准宏，避免动态分配冲突）
    uint64 kstack_va = KSTACK(0);
    // 验证内核栈地址合法性（必须在TRAPFRAME之下，符合内存布局）
    assert(kstack_va < TRAPFRAME, "alloc_kstack_page: invalid kstack address");
    // 内核栈物理页分配（内核空间用pmem_alloc(true)）
    void *kstack_pa = pmem_alloc(true);
    assert(kstack_pa != NULL, "alloc_kstack_page: pmem_alloc failed");
    memset(kstack_pa, 0, PGSIZE);
    // 内核栈恒等映射（VA == PA，系统规范）
    vm_mappages(kernel_pgtbl, kstack_va, (uint64)kstack_pa, PGSIZE, PTE_R | PTE_W);
    return kstack_va;
}
/**
 * 为用户进程创建页表（最小初始化：映射trampoline + trapframe）
 */
pgtbl_t proc_pgtbl_init(uint64 trapframe_pa)
{
    // 分配用户页表根页（内核空间分配，pmem_alloc(true)）
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: root pgtbl alloc failed");
    memset(pgtbl, 0, PGSIZE);

    // 1. 映射trampoline（RX权限，S态专用，无PTE_U）
    // TRAMPOLINE是全局标准VA（高地址），trampoline是物理地址（内核代码段）
    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 2. 映射trapframe（RW权限，S态专用，无PTE_U）
    // TRAPFRAME是全局标准VA（高地址），trapframe_pa是物理地址
    vm_mappages(pgtbl, TRAPFRAME, trapframe_pa, PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}

/**
 * 创建并切入第一个用户进程 proczero（严格对齐全局内存布局）
 */
void proc_make_first()
{

    #define USTACK (TRAPFRAME - PGSIZE)
    proc_t *p = &proczero;
    memset(p, 0, sizeof(proc_t));
    p->pid = 0;          // 进程0的PID
    p->mmap = NULL;      // 初始无mmap区域

    // 1. 分配并初始化trapframe（内核物理页，仅内核访问）
    void *tf_pa = pmem_alloc(true);
    assert(tf_pa != NULL, "proc_make_first: trapframe alloc failed");
    p->tf = (trapframe_t *)tf_pa; // trapframe VA == PA（内核恒等映射）

    // 2. 创建用户页表（映射trampoline + trapframe）
    p->pgtbl = proc_pgtbl_init((uint64)tf_pa);

    // 分配用户代码页放在 USER_BASE
    void *ucode_pa = pmem_alloc(false);
    assert(ucode_pa != NULL && (uint64)ucode_pa != 0, "proc_make_first: initcode page alloc failed");
    memset(ucode_pa, 0, PGSIZE); // 先清零物理页

    // 用 memmove 拷贝用户初始化代码（兼容内存重叠，内核态直接操作物理页）
    uint32 copy_len = (target_user_initcode_len > PGSIZE) ? PGSIZE : target_user_initcode_len;
    if (copy_len > 0 && target_user_initcode != NULL) {
        memmove(ucode_pa, (void*)target_user_initcode, copy_len);
    }
    // 映射用户代码页（vm_mappages无返回值，前置校验参数合法性）
    assert(p->pgtbl != NULL && USER_BASE != 0 && (uint64)ucode_pa != 0, 
           "proc_make_first: ucode map param invalid");
    vm_mappages(p->pgtbl, USER_BASE, (uint64)ucode_pa, PGSIZE, PTE_R | PTE_X | PTE_U);

    // 分配用户栈（取消USTACK_TOP/USTACK_BOT，改用USTACK宏）
    void *ustack_pa = pmem_alloc(false);
    assert(ustack_pa != NULL && (uint64)ustack_pa != 0, "proc_make_first: ustack page alloc failed");
    memset(ustack_pa, 0, PGSIZE); // 安全清零用户栈

    // 映射用户栈页（使用USTACK宏替代原USTACK_BOT）
    assert(p->pgtbl != NULL && USTACK != 0 && (uint64)ustack_pa != 0,
           "proc_make_first: ustack map param invalid");
    vm_mappages(p->pgtbl, USTACK, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_npage = 1;
    p->heap_top = USER_BASE + PGSIZE; // 对齐参考版的堆顶初始化

    // 5. 初始化进程内核栈和上下文
    p->kstack = alloc_kstack_page(); // 内核栈VA（KSTACK(0)）
    p->ctx.sp = p->kstack + PGSIZE;  // 内核栈顶（空栈状态）
    p->ctx.ra = (uint64)trap_user_return; // 切换后执行trap_user_return

    // 6. 配置trapframe（用户态切内核态的关键字段）
    p->tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl); // 内核页表SATP
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;        // 内核栈顶（进入内核时使用）
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler; // 用户态trap处理入口
    p->tf->user_to_kern_hartid = r_tp();                // 当前CPU核心ID
    p->tf->user_to_kern_epc = USER_BASE;                // 首次进入用户态的PC（initcode入口）
    p->tf->sp = USTACK + PGSIZE;                              // 用户态初始栈顶（向下增长）

    // 7. 当前CPU绑定进程0
    cpu_t *c = mycpu();
    c->proc = p;

    // 切入用户进程：先切换页表，再通过trap_user_return返回用户态
    trap_user_return(); // 核心切换逻辑（汇编实现：恢复tf寄存器 + sret）

    // 正常情况下不会执行到这里（用户态无限循环）
    panic("proc_make_first: unexpected return from user mode");
}