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

/* ------------本地变量----------- */

// 进程结构体数组 + 第一个用户进程的指针
static proc_t proc_list[N_PROC];
static proc_t *proczero;

// 全局pid + 保护它的锁
static int global_pid;
static spinlock_t pid_lk;

/* 获取一个pid */
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&pid_lk);
    assert(global_pid > 0, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&pid_lk);
    return tmp;
}

/* 释放进程锁 + trap_user_return */
static void proc_return()
{

}

/* 进程模块初始化 */
void proc_init()
{    

}

/* 
    申请一个UNUSED进程结构体(返回时带锁)
    并执行通用的初始化逻辑
*/
proc_t *proc_alloc()
{

}

/* 
    回收一个进程结构体并释放它包含的资源
    tips: 调用者需要持有进程锁
*/
void proc_free(proc_t *p)
{

}
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
    proc_t *p = &proczero;
    memset(p, 0, sizeof(proc_t));
    p->pid = 0;          // 进程0的PID
    p->mmap = NULL;      // 初始无mmap区域

    // 1. 分配并初始化trapframe（内核物理页，仅内核访问）
    void *tf_pa = pmem_alloc(true);
    assert(tf_pa != NULL, "proc_make_first: trapframe alloc failed");
    memset(tf_pa, 0, PGSIZE);
    p->tf = (trapframe_t *)tf_pa; // trapframe VA == PA（内核恒等映射）

    // 2. 创建用户页表（映射trampoline + trapframe）
    p->pgtbl = proc_pgtbl_init((uint64)tf_pa);

    // 3. 装载用户代码（initcode）到 USER_BASE（全局标准用户代码VA：0x1000）
    void *ucode_pa = pmem_alloc(false); // 用户空间物理页（pmem_alloc(false)）
    assert(ucode_pa != NULL, "proc_make_first: initcode page alloc failed");
    memset(ucode_pa, 0, PGSIZE);
    // 复制initcode到用户物理页（限制大小不超过1页）
    uint32 copy_len = (target_user_initcode_len > PGSIZE) ? PGSIZE : target_user_initcode_len;
    memmove(ucode_pa, target_user_initcode, copy_len);
    // 映射用户代码页（RXU权限：用户态可执行、可读）
    vm_mappages(p->pgtbl, USER_BASE, (uint64)ucode_pa, PGSIZE, PTE_R | PTE_X | PTE_U);

    // 4. 初始化用户栈（1页，全局标准布局：栈顶=USER_BASE + 8*PGSIZE）
    const uint64 USTACK_TOP = USER_BASE + 8 * PGSIZE; // 与原PROC0_USTACK_TOP一致，复用全局宏
    const uint64 USTACK_BOT = USTACK_TOP - PGSIZE;    // 1页栈，栈底=栈顶-4KB
    // 分配用户栈物理页
    void *ustack_pa = pmem_alloc(false);
    assert(ustack_pa != NULL, "proc_make_first: ustack page alloc failed");
    memset(ustack_pa, 0, PGSIZE);
    // 映射用户栈页（RWU权限：用户态可读写）
    vm_mappages(p->pgtbl, USTACK_BOT, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    // 更新进程栈信息
    p->ustack_npage = 1;          // 初始1页栈（符合要求）
    p->heap_top = USTACK_BOT;     // 堆顶=栈底（系统约定：堆向上、栈向下）

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
    p->tf->sp = USTACK_TOP;                              // 用户态初始栈顶（向下增长）

    // 7. 当前CPU绑定进程0
    cpu_t *c = mycpu();
    c->proc = p;

    // 打印调试信息（对齐原格式，使用全局宏确保地址正确）
    printf("[proc] switch to user: epc=%p, usp=%p, ksp=%p\n",
           (void*)USER_BASE, (void*)USTACK_TOP, (void*)(p->kstack + PGSIZE));

    // 切入用户进程：先切换页表，再通过trap_user_return返回用户态
    trap_user_return(); // 核心切换逻辑（汇编实现：恢复tf寄存器 + sret）

    // 正常情况下不会执行到这里（用户态无限循环）
    panic("proc_make_first: unexpected return from user mode");
}

/*
    父进程产生子进程
    UNUSED -> RUNNABLE
*/
int proc_fork()
{

}

/*
    进程主动放弃CPU控制权
    RUNNING->RUNNABLE
*/
void proc_yield()
{

}

/*
    当父进程退出时, 让它的所有子进程认proczero为父
    因为proczero永不退出, 可以回收子进程的资源
*/
static void proc_reparent(proc_t *parent)
{

}

/*
    唤醒等待呼叫的进程
    由proc_exit调用
    tips: 调用者需要持有p的进程锁
*/
static void proc_try_wakeup(proc_t *p)
{

}

/*
    进程退出
    RUNNING -> ZOMBIE
*/
void proc_exit(int exit_code)
{

}

/*
    父进程等待一个子进程进入ZOMBIE状态
    1. 如果等到: 释放子进程, 返回子进程的pid, 将子进程的退出状态传出到user_addr
    2. 如果发现没孩子: 返回-1
    3. 如果没等到: 父进程进入睡眠状态 
*/
int proc_wait(uint64 user_addr)
{

}

/*
    进程等待sleep_space对应的资源, 进入睡眠状态
    RUNNING -> SLEEPING
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{

}

/*
    唤醒所有等待sleep_space的进程
    SLEEPING -> RUNNABLE
*/
void proc_wakeup(void *sleep_space)
{

}

/* 
    用户进程切换到调度器
    tips: 调用者保证持有当前进程的锁
*/
void proc_sched()
{

}

/* 
    调度器
    RUNNABLE->RUNNING
*/
void proc_scheduler()
{

}