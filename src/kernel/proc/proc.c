// src/kernel/proc/proc.c
#include "mod.h"
#include "method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../lib/method.h"
#include "../lib/type.h"
#include "../trap/mod.h"

// 这个文件通过make build生成, 是proczero对应的ELF文件
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return(void);
extern void trap_user_handler(void);

// 第一个用户进程
static proc_t proczero;

// 内核页表（在 mem/kvm.c 中定义）
extern pgtbl_t kernel_pgtbl;

// 移除这里的重复定义，使用 proc/method.h 中的定义
// 不要重新定义 USER_CODE_VA, USER_HEAP_TOP, USER_STACK_TOP

// 获得一个初始化过的用户页表
// 完成trapframe和trampoline的映射
pgtbl_t proc_pgtbl_init(uint64 trapframe)
{
    // 分配根页表
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: root alloc failed");
    memset(pgtbl, 0, PGSIZE);

    printf("[proc_pgtbl_init] creating user page table at %p\n", pgtbl);

    // 1. 映射 trampoline 页（ supervisor only ）
    uint64 tramp_va = (uint64)ALIGN_DOWN((uint64)trampoline, PGSIZE);
    uint64 tramp_pa = tramp_va; // 内核恒等映射
    vm_mappages(pgtbl, tramp_va, tramp_pa, PGSIZE, PTE_R | PTE_X);
    printf("  trampoline: va=%p -> pa=%p (flags=R|X)\n", 
           (void*)tramp_va, (void*)tramp_pa);

    // 2. 映射 trapframe 页（ supervisor only ）
    uint64 tf_va = TRAPFRAME;
    uint64 tf_pa = trapframe;
    vm_mappages(pgtbl, tf_va, tf_pa, PGSIZE, PTE_R | PTE_W);
    printf("  trapframe: va=%p -> pa=%p (flags=R|W)\n", 
           (void*)tf_va, (void*)tf_pa);

    return pgtbl;
}

/*
    第一个用户态进程的创建
    它的代码和数据位于initcode.h的initcode数组

    第一个进程的用户地址空间布局:
    trapoline   (1 page)
    trapframe   (1 page)
    ustack      (1 page)
    .......
                        <--heap_top
    code + data (1 page)
    empty space (1 page) 最低的4096字节 不分配物理页，同时不可访问

	注意: 用用户空间的地址映射需要标记 PTE_U
*/
void proc_make_first()
{
    // 清零进程结构体
    memset(&proczero, 0, sizeof(proczero));
    proczero.pid = 0;

    printf("[proc_make_first] creating first user process\n");
    printf("  user layout: code=%p, heap_top=%p, stack_top=%p\n",
           (void*)USER_CODE_VA, (void*)USER_HEAP_TOP, (void*)USER_STACK_TOP);

    // 1. 分配 trapframe（内核物理页）
    proczero.tf = (trapframe_t *)pmem_alloc(true);
    assert(proczero.tf != NULL, "proc_make_first: trapframe alloc failed");
    memset(proczero.tf, 0, PGSIZE);
    printf("  trapframe allocated at %p\n", proczero.tf);

    // 2. 创建用户页表
    proczero.pgtbl = proc_pgtbl_init((uint64)proczero.tf);

    // 3. 映射用户代码页
    assert(initcode_len <= PGSIZE, "initcode too large (>1 page)");
    void *ucode_page = pmem_alloc(false);
    assert(ucode_page != NULL, "proc_make_first: ucode alloc failed");
    memset(ucode_page, 0, PGSIZE);
    memmove(ucode_page, initcode, (uint32)initcode_len);
    
    vm_mappages(proczero.pgtbl, USER_CODE_VA, (uint64)ucode_page, PGSIZE, 
                PTE_R | PTE_X | PTE_U);
    printf("  user code: va=%p -> pa=%p (flags=R|X|U)\n", 
           (void*)USER_CODE_VA, ucode_page);

    // 4. 映射用户栈页
    void *ustack_page = pmem_alloc(false);
    assert(ustack_page != NULL, "proc_make_first: ustack alloc failed");
    memset(ustack_page, 0, PGSIZE);
    
    uint64 ustack_va = USER_HEAP_TOP; // 栈底
    vm_mappages(proczero.pgtbl, ustack_va, (uint64)ustack_page, PGSIZE, 
                PTE_R | PTE_W | PTE_U);
    printf("  user stack: va=%p -> pa=%p (flags=R|W|U)\n", 
           (void*)ustack_va, ustack_page);

    proczero.ustack_npage = 1;
    proczero.heap_top = USER_HEAP_TOP; // 堆顶在栈底
    proczero.mmap = NULL;

    // 5. 分配内核栈
    proczero.kstack = (uint64)pmem_alloc(true);
    assert(proczero.kstack != 0, "proc_make_first: kstack alloc failed");
    memset((void*)proczero.kstack, 0, PGSIZE);
    printf("  kernel stack at %p\n", (void*)proczero.kstack);

    // 6. 设置上下文
    memset(&proczero.ctx, 0, sizeof(proczero.ctx));
    proczero.ctx.ra = (uint64)trap_user_return;
    proczero.ctx.sp = proczero.kstack + PGSIZE; // 栈顶

    // 7. 设置 trapframe 的回内核信息
    proczero.tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);
    proczero.tf->user_to_kern_sp = proczero.kstack + PGSIZE;
    proczero.tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    proczero.tf->user_to_kern_hartid = r_tp();

    // 8. 设置用户态初始寄存器
    proczero.tf->user_to_kern_epc = USER_CODE_VA; // 用户程序入口
    proczero.tf->sp = USER_STACK_TOP;             // 用户栈顶
    
    // a0 寄存器在 trapframe 中是 saved_sscratch 字段
    proczero.tf->saved_sscratch = 0;              // argc = 0
    proczero.tf->a1 = 0;                          // argv = 0
    proczero.tf->a2 = 0;                          // envp = 0

    printf("  user entry: epc=%p, usp=%p, a0=%ld\n", 
           (void*)proczero.tf->user_to_kern_epc, 
           (void*)proczero.tf->sp,
           proczero.tf->saved_sscratch);

    // 9. 切换到第一个进程
    cpu_t *c = mycpu();
    c->proc = &proczero;

    printf("[proc_make_first] switching to user process...\n");
    
    // 保存调试信息
    printf("  c->ctx.ra=%p, proc->ctx.ra=%p\n", 
           (void*)c->ctx.ra, (void*)proczero.ctx.ra);
    
    swtch(&c->ctx, &proczero.ctx);
    
    // 如果回到这里，说明出错了
    printf("[proc_make_first] ERROR: returned from swtch!\n");
    panic("should not return from first user process");
}