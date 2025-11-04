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
void proc_make_first() {
    // 清零进程结构体（proczero是第一个进程的全局变量）
    memset(&proczero, 0, sizeof(proczero));
    proczero.pid = 0;

    printf("[proc_make_first] creating first user process\n");
    printf("  user layout: code=%p, heap_top=%p, stack_bot=%p, stack_top=%p\n",
           (void*)USER_CODE_VA, (void*)USER_HEAP_TOP,
           (void*)USER_STACK_BOT, (void*)USER_STACK_TOP);

    // 1. 分配 trapframe（内核物理页，用于保存用户态->内核态的上下文）
    proczero.tf = (trapframe_t *)pmem_alloc(true);
    assert(proczero.tf != NULL, "proc_make_first: trapframe alloc failed");
    memset(proczero.tf, 0, PGSIZE);
    printf("  trapframe allocated at %p (pa)\n", proczero.tf);

    // 2. 创建用户页表（映射trampoline、trapframe等关键区域）
    proczero.pgtbl = proc_pgtbl_init((uint64)proczero.tf);
    assert(proczero.pgtbl != NULL, "proc_pgtbl_init failed");
    printf("  user page table created at %p (pa)\n", proczero.pgtbl);

    // 3. 映射用户代码页（加载initcode二进制）
    #include "../../user/initcode.h"  // 引入用户程序二进制数据
    assert(initcode_len <= PGSIZE, "initcode too large (>1 page)");
    void *ucode_page = pmem_alloc(false);
    assert(ucode_page != NULL, "proc_make_first: ucode alloc failed");
    memset(ucode_page, 0, PGSIZE);
    memmove(ucode_page, initcode, (uint32)initcode_len);  // 拷贝用户程序到物理页
    printf("vm_mappages args: pgtbl=%p, va=%p, pa=%p, size=%d, perm=0x%x\n",
       proczero.pgtbl, (void*)USER_CODE_VA, ucode_page, PGSIZE, PTE_R | PTE_X | PTE_U);
    int code_map_ret = vm_mappages(proczero.pgtbl, USER_CODE_VA, (uint64)ucode_page, PGSIZE, 
                PTE_R | PTE_X | PTE_U);  // 代码页：可读可执行+用户态
    assert(code_map_ret == 0, "proc_make_first: ucode map failed");
    printf("  user code: va=%p -> pa=%p (flags=R|X|U)\n", 
           (void*)USER_CODE_VA, ucode_page);

    // 4. 映射用户栈页（栈底虚拟地址为USER_STACK_BOT）
    void *ustack_page = pmem_alloc(false);
    assert(ustack_page != NULL, "proc_make_first: ustack alloc failed");
    memset(ustack_page, 0, PGSIZE);
    
    uint64 ustack_va = USER_STACK_BOT;
    int stack_map_ret = vm_mappages(proczero.pgtbl, ustack_va, (uint64)ustack_page, PGSIZE, 
                PTE_R | PTE_W | PTE_U);  // 栈页：可读可写+用户态
    assert(stack_map_ret == 0, "proc_make_first: ustack map failed");
    printf("  user stack: va=%p -> pa=%p (flags=R|W|U)\n", 
           (void*)ustack_va, ustack_page);

    // 初始化进程的内存管理字段
    proczero.ustack_npage = 1;  // 用户栈占用1页
    proczero.heap_top = USER_HEAP_TOP;  // 堆顶初始地址
    proczero.mmap = NULL;  // 无mmap区域

    // 5. 分配内核栈（进程在 kernel 态执行时使用的栈）
    proczero.kstack = (uint64)pmem_alloc(true);
    assert(proczero.kstack != 0, "proc_make_first: kstack alloc failed");
    assert((proczero.kstack & (PGSIZE - 1)) == 0, "kstack not page-aligned");
    memset((void*)proczero.kstack, 0, PGSIZE);
    printf("  kernel stack at %p (va, size=1 page)\n", (void*)proczero.kstack);

    // 6. 设置内核态上下文（切换到用户态的准备）
    memset(&proczero.ctx, 0, sizeof(proczero.ctx));
    proczero.ctx.ra = (uint64)trap_user_return;  // 切换后执行trap_user_return
    proczero.ctx.sp = proczero.kstack + PGSIZE;  // 内核栈顶（指向页末端）

    // 7. 设置 trapframe 中用户态->内核态的关键信息
    proczero.tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);  // 内核页表
    proczero.tf->user_to_kern_sp = proczero.kstack + PGSIZE;   // 内核栈顶
    proczero.tf->user_to_kern_trapvector = (uint64)trap_user_handler;  // 用户态陷阱处理函数
    proczero.tf->user_to_kern_hartid = r_tp();  // 当前CPU核心ID

    // 8. 设置用户态初始寄存器（进入用户程序时的状态）
    proczero.tf->user_to_kern_epc = USER_CODE_VA;  // 程序入口（用户代码起始地址）
    proczero.tf->sp = USER_STACK_TOP;              // 用户栈顶（栈指针初始值）
    // a0~a7寄存器初始化（根据trapframe结构体定义）
    proczero.tf->a0 = 0;  // a0 = 0（argc=0）
    proczero.tf->a1 = 0;              // a1 = 0（argv=NULL）
    proczero.tf->a2 = 0;              // a2 = 0（envp=NULL）
    proczero.tf->a3 = 0;
    proczero.tf->a4 = 0;
    proczero.tf->a5 = 0;
    proczero.tf->a6 = 0;
    proczero.tf->a7 = 0;

    printf("  user entry: epc=%p, usp=%p, a0=%ld\n", 
           (void*)proczero.tf->user_to_kern_epc, 
           (void*)proczero.tf->sp,
           proczero.tf->a0);

    // 9. 切换到第一个用户进程
    cpu_t *c = mycpu();
    c->proc = &proczero;  // 当前CPU绑定到proczero

    printf("[proc_make_first] switching to user process...\n");
    printf("  c->ctx.ra=%p, proc->ctx.ra=%p\n", 
           (void*)c->ctx.ra, (void*)proczero.ctx.ra);
    
    swtch(&c->ctx, &proczero.ctx);  // 上下文切换，进入用户态
    
    // 正常情况下不会执行到这里（切换后直接进入用户程序）
    printf("[proc_make_first] ERROR: returned from swtch!\n");
    panic("should not return from first user process");
}