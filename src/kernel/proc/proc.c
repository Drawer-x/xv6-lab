#include "mod.h"
#include "method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../lib/method.h"
#include "../lib/type.h"
#include "../../user/initcode.h"

// 外部符号声明（必须实现的汇编上下文切换函数）
extern void swtch(context_t *old_ctx, context_t *new_ctx);
// 中断控制函数（假设系统已实现）
extern void intr_off();
extern void intr_on();
// 获取当前CPU结构体（假设系统已实现，仅需其中的proc字段）
extern cpu_t *mycpu();

// 其他外部声明（保持不变）
extern pgtbl_t kernel_pgtbl;
extern char trampoline[];
extern void user_return(trapframe_t *tf, uint64 satp);
extern void trap_user_handler();
extern void trap_user_return();

// 补充缺失的汇编辅助宏（若头文件未定义）
#ifndef r_tp
#define r_tp() ({ uint64 x; asm volatile("mv %0, tp" : "=r"(x)); x; })  // 获取当前Hart ID
#endif

#ifndef w_satp
#define w_satp(x) asm volatile("csrw satp, %0" : : "r"(x))  // 写入SATP寄存器
#endif

#ifndef sfence_vma
#define sfence_vma() asm volatile("sfence.vma" : : : "memory")  // 刷新TLB
#endif

/* ------------本地变量----------- */
static proc_t proc_list[N_PROC];
static proc_t *proczero;
static int global_pid;
static spinlock_t pid_lk;

/* 提前声明静态函数（解决顺序依赖问题） */
static void proc_try_wakeup(proc_t *p);
static int alloc_pid();
static void proc_return();

/* 获取一个pid */
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&pid_lk);
    assert(global_pid > 0 && global_pid <= N_PROC, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&pid_lk);
    return tmp;
}

/* 释放进程锁 + trap_user_return */
static void proc_return()
{
    // 进程从内核态返回用户态的入口（由swtch恢复后执行）
    proc_t *p = mycpu()->proc;
    assert(p != NULL, "proc_return: no running proc");
    // 切换回用户态
    user_return(p->tf, MAKE_SATP(p->pgtbl));
}

/* 进程模块初始化 */
void proc_init()
{
    spinlock_init(&pid_lk, "pid_lock");
    global_pid = 1;

    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        p->state = UNUSED;
        spinlock_init(&p->lk, "proc_lock");
        // 替代 memset：用uvm_copyin填充0
        uvm_copyin(kernel_pgtbl, (uint64)p->name, 0, sizeof(p->name));
        p->parent = NULL;
        p->exit_code = 0;
        p->sleep_space = NULL;
        p->pgtbl = NULL;
        p->heap_top = 0;
        p->ustack_npage = 0;
        p->mmap = NULL;
        p->tf = NULL;
        p->kstack = 0;
        uvm_copyin(kernel_pgtbl, (uint64)&p->ctx, 0, sizeof(context_t));
    }
}

/* 申请一个UNUSED进程结构体(返回时带锁) */
proc_t *proc_alloc()
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        spinlock_acquire(&p->lk);
        
        if (p->state == UNUSED) {
            p->pid = (i == 0) ? 0 : alloc_pid();
            // 替代 memset：用uvm_copyin填充0
            uvm_copyin(kernel_pgtbl, (uint64)p->name, 0, sizeof(p->name));
            p->state = RUNNABLE;
            p->parent = NULL;
            p->exit_code = 0;
            p->sleep_space = NULL;
            p->mmap = NULL;
            p->heap_top = 0;
            p->ustack_npage = 0;
            p->pgtbl = NULL;
            p->tf = NULL;
            p->kstack = 0;
            uvm_copyin(kernel_pgtbl, (uint64)&p->ctx, 0, sizeof(context_t));
            p->ctx.ra = (uint64)proc_return; // 进程内核态返回入口
            
            return p;
        }
        
        spinlock_release(&p->lk);
    }
    return NULL;
}

/* 回收一个进程结构体并释放它包含的资源 */
void proc_free(proc_t *p)
{
    assert(p != NULL, "proc_free: NULL pointer");
    assert(spinlock_holding(&p->lk), "proc_free: not holding proc lock");
    assert(p->state == ZOMBIE || p->state == UNUSED, "proc_free: invalid state");

    if (p->pgtbl != NULL) {
        uvm_destroy_pgtbl(p->pgtbl);
        p->pgtbl = NULL;
    }

    if (p->tf != NULL) {
        // 修复：void* → uint64（符合pmem_free参数要求）
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
    }

    mmap_region_t *mmap = p->mmap;
    while (mmap != NULL) {
        mmap_region_t *next = mmap->next;
        for (uint32 i = 0; i < mmap->npages; i++) {
            uint64 va = mmap->begin + i * PGSIZE;
            pte_t *pte = vm_getpte(p->pgtbl, va, false);
            if (pte != NULL && (*pte & PTE_V)) {
                // 修复：void* → uint64（PTE_TO_PA返回物理地址整数）
                pmem_free((uint64)PTE_TO_PA(*pte), false);
            }
        }
        // 替代 memset：用uvm_copyin填充0
        uvm_copyin(kernel_pgtbl, (uint64)mmap, 0, sizeof(mmap_region_t));
        mmap = next;
    }
    p->mmap = NULL;

    p->state = UNUSED;
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    uvm_copyin(kernel_pgtbl, (uint64)p->name, 0, sizeof(p->name));
    uvm_copyin(kernel_pgtbl, (uint64)&p->ctx, 0, sizeof(context_t));
    p->kstack = 0;
}

/** 为用户进程创建页表 */
pgtbl_t proc_pgtbl_init(uint64 trapframe_pa)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: root pgtbl alloc failed");
    // 替代 memset：用uvm_copyin填充0（内核页表访问，src=0表示空地址）
    uvm_copyin(kernel_pgtbl, (uint64)pgtbl, 0, PGSIZE);

    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    vm_mappages(pgtbl, TRAPFRAME, trapframe_pa, PGSIZE, PTE_R | PTE_W);

    return pgtbl;
}

/** 创建并初始化第一个用户进程 proczero */
void proc_make_first()
{
    proczero = proc_alloc();
    assert(proczero != NULL, "proc_make_first: proczero alloc failed");
    proc_t *p = proczero;

    // 替代 strncpy：用uvm_copyin拷贝进程名称（内核态→内核态）
    uvm_copyin(kernel_pgtbl, (uint64)p->name, (uint64)"proczero", sizeof(p->name)-1);
    p->name[sizeof(p->name)-1] = '\0'; // 手动添加字符串终止符

    void *tf_pa = pmem_alloc(true);
    assert(tf_pa != NULL, "proc_make_first: trapframe alloc failed");
    // 替代 memset：用uvm_copyin填充0
    uvm_copyin(kernel_pgtbl, (uint64)tf_pa, 0, PGSIZE);
    p->tf = (trapframe_t *)tf_pa;

    p->pgtbl = proc_pgtbl_init((uint64)tf_pa);

    void *ucode_pa = pmem_alloc(false);
    assert(ucode_pa != NULL, "proc_make_first: initcode page alloc failed");
    // 替代 memset：用uvm_copyin填充0
    uvm_copyin(kernel_pgtbl, (uint64)ucode_pa, 0, PGSIZE);
    uint32 copy_len = (target_user_initcode_len > PGSIZE) ? PGSIZE : target_user_initcode_len;
    // 替代 memmove：用uvm_copyin拷贝初始化代码（内核态→用户态物理页）
    uvm_copyin(kernel_pgtbl, (uint64)ucode_pa, (uint64)target_user_initcode, copy_len);
    vm_mappages(p->pgtbl, USER_BASE, (uint64)ucode_pa, PGSIZE, PTE_R | PTE_X | PTE_U);

    const uint64 USTACK_TOP = USER_BASE + 8 * PGSIZE;
    const uint64 USTACK_BOT = USTACK_TOP - PGSIZE;
    void *ustack_pa = pmem_alloc(false);
    assert(ustack_pa != NULL, "proc_make_first: ustack page alloc failed");
    // 替代 memset：用uvm_copyin填充0
    uvm_copyin(kernel_pgtbl, (uint64)ustack_pa, 0, PGSIZE);
    vm_mappages(p->pgtbl, USTACK_BOT, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_npage = 1;
    p->heap_top = USTACK_BOT;

    p->kstack = KSTACK(0);
    p->ctx.sp = p->kstack + PGSIZE; // 内核栈顶（栈向下生长）
    p->ctx.ra = (uint64)trap_user_return; // 首次切换后进入用户态

    // 初始化陷阱帧（用户态→内核态切换上下文）
    p->tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_epc = USER_BASE; // 用户态入口地址（initcode起始）
    p->tf->user_to_kern_hartid = r_tp();
    p->tf->sp = USTACK_TOP; // 用户栈顶

    cpu_t *c = mycpu();
    c->proc = p;

    printf("[proc] switch to user: epc=%p, usp=%p, ksp=%p\n",
           (void*)USER_BASE, (void*)USTACK_TOP, (void*)(p->kstack + PGSIZE));

    spinlock_release(&p->lk);
}

/*
    proc_fork: 父进程创建子进程
    功能：复制父进程状态，建立父子关系，子进程返回0，父进程返回子进程PID
*/
int proc_fork()
{
    cpu_t *c = mycpu();
    proc_t *parent = c->proc;
    assert(parent != NULL && parent->state == RUNNING, "proc_fork: parent not running");

    // 1. 申请空闲进程结构体（返回时带锁）
    proc_t *child = proc_alloc();
    if (child == NULL) {
        return -1; // 无空闲进程
    }

    // 2. 复制父进程状态（持有child->lk锁，保证原子性）
    // 复制进程名称（替代strncpy）
    uvm_copyin(kernel_pgtbl, (uint64)child->name, (uint64)parent->name, sizeof(child->name)-1);
    child->name[sizeof(child->name)-1] = '\0'; // 手动添加终止符

    // 复制用户态页表（使用内存模块提供的uvm_copy_pgtbl，完整拷贝父进程页表）
    child->pgtbl = proc_pgtbl_init((uint64)child->tf);
    uvm_copy_pgtbl(parent->pgtbl, child->pgtbl, parent->heap_top, parent->ustack_npage, parent->mmap);

    // 复制堆、栈信息
    child->heap_top = parent->heap_top;
    child->ustack_npage = parent->ustack_npage;
    // 复制mmap区域（简化实现：暂不处理，后续可扩展为链表拷贝）
    child->mmap = parent->mmap;

    // 复制陷阱帧（替代memcpy）
    if (parent->tf != NULL && child->tf != NULL) {
        uvm_copyin(kernel_pgtbl, (uint64)child->tf, (uint64)parent->tf, sizeof(trapframe_t));
        child->tf->sp = parent->tf->sp; // 子进程用户栈顶与父进程一致
    }

    // 复制内核栈（替代memcpy：内核态→内核态拷贝）
    child->kstack = KSTACK(child->pid); // 按PID分配独立内核栈
    uvm_copyin(kernel_pgtbl, (uint64)child->kstack, (uint64)parent->kstack, 2*PGSIZE);
    // 调整子进程内核栈指针（保持与父进程相同的栈偏移）
    child->ctx = parent->ctx;
    child->ctx.sp = child->kstack + (parent->ctx.sp - parent->kstack);

    // 3. 建立父子关系
    child->parent = parent;
    // 4. 设置子进程返回值为0（用户态通过a0寄存器获取）
    child->tf->a0 = 0;

    // 5. 解锁子进程，状态已设为RUNNABLE（proc_alloc中初始化）
    spinlock_release(&child->lk);

    // 父进程返回子进程PID
    return child->pid;
}

/*
    proc_exit: 进程退出
    功能：将进程设为ZOMBIE态，设置退出码，唤醒父进程，处理子进程过继
*/
void proc_exit(int exit_code)
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    assert(p != NULL && p->state == RUNNING, "proc_exit: not running");

    spinlock_acquire(&p->lk);

    // 1. 设置进程状态为ZOMBIE，保存退出码
    p->state = ZOMBIE;
    p->exit_code = exit_code;

    // 2. 唤醒父进程（父进程可能在proc_wait中睡眠）
    proc_try_wakeup(p);

    // 3. 处理子进程过继：若当前进程有子进程，将子进程过继给proczero
    for (int i = 0; i < N_PROC; i++) {
        proc_t *child = &proc_list[i];
        spinlock_acquire(&child->lk);
        if (child->state != UNUSED && child->parent == p) {
            child->parent = proczero; // 过继给根进程proczero
        }
        spinlock_release(&child->lk);
    }

    // 4. 释放CPU，切换到调度器（进程已退出，不再执行）
    c->proc = NULL;
    spinlock_release(&p->lk);
    proc_sched();

    // 永远不会执行到这里
    panic("proc_exit: should not return");
}

/*
    proc_wait: 父进程等待子进程退出
    功能：循环扫描子进程，找到ZOMBIE态子进程并回收，返回子进程PID；若未找到则睡眠
*/
int proc_wait(uint64 user_addr)
{
    cpu_t *c = mycpu();
    proc_t *parent = c->proc;
    assert(parent != NULL && parent->state == RUNNING, "proc_wait: parent not running");

    while (1) {
        intr_off();// 禁用中断，避免扫描被打断

        // 1. 扫描所有进程，查找当前进程的ZOMBIE态子进程
        for (int i = 0; i < N_PROC; i++) {
            proc_t *child = &proc_list[i];
            spinlock_acquire(&child->lk);

            if (child->state == ZOMBIE && child->parent == parent) {
                // 找到目标子进程，记录PID和退出码
                int pid = child->pid;
                int exit_code = child->exit_code;

                // 2. 回收子进程资源
                proc_free(child);
                spinlock_release(&child->lk);
                intr_on();

                // 3. 将退出码写入用户态地址（用uvm_copyout确保安全）
                if (user_addr != 0) {
                    uvm_copyout(parent->pgtbl, user_addr, (uint64)&exit_code, sizeof(int));
                }

                return pid; // 返回子进程PID
            }

            spinlock_release(&child->lk);
        }

        // 2. 未找到ZOMBIE子进程，进入睡眠态（等待子进程退出唤醒）
        proc_sleep(parent, &parent->lk); // 睡眠资源设为自身，父进程等待被唤醒
        intr_on();
    }
}

/*
    proc_sleep: 当前进程睡眠，等待指定资源
    参数：sleep_space - 等待的资源标识；lock - 当前持有的锁（需释放后睡眠）
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    assert(p != NULL && p->state == RUNNING, "proc_sleep: not running");
    assert(spinlock_holding(lock), "proc_sleep: not holding lock");

    spinlock_acquire(&p->lk);

    // 1. 绑定睡眠资源，设置状态为SLEEPING
    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    // 2. 释放传入的锁（避免死锁：睡眠时不持有其他锁）
    spinlock_release(lock);

    // 3. 切换到调度器（睡眠进程不再占用CPU）
    c->proc = NULL;
    spinlock_release(&p->lk);
    proc_sched();

    // 4. 被唤醒后，重新获取传入的锁（恢复睡眠前的锁状态）
    spinlock_acquire(lock);
}

/*
    proc_wakeup: 唤醒所有等待指定资源的进程
    参数：sleep_space - 资源标识（与proc_sleep的参数一致）
*/
void proc_wakeup(void *sleep_space)
{
    intr_off(); // 禁用中断，避免唤醒过程被打断

    // 扫描所有进程，唤醒等待该资源的SLEEPING进程
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        spinlock_acquire(&p->lk);

        if (p->state == SLEEPING && p->sleep_space == sleep_space) {
            // 唤醒：设置为RUNNABLE态，清空睡眠资源
            p->state = RUNNABLE;
            p->sleep_space = NULL;
        }

        spinlock_release(&p->lk);
    }

    intr_on();
}

/*
    proc_try_wakeup: 专门唤醒当前进程的父进程（用于proc_exit）
    参数：p - 当前进程（子进程）
*/
static void proc_try_wakeup(proc_t *p)
{
    assert(p != NULL && spinlock_holding(&p->lk), "proc_try_wakeup: invalid state");

    // 1. 获取父进程，无父进程则唤醒proczero
    proc_t *parent = p->parent;
    if (parent == NULL) {
        parent = proczero;
    }

    // 2. 唤醒父进程（父进程可能在proc_wait中睡眠，睡眠资源为自身）
    spinlock_acquire(&parent->lk);
    if (parent->state == SLEEPING && parent->sleep_space == parent) {
        parent->state = RUNNABLE;
        parent->sleep_space = NULL;
    }
    spinlock_release(&parent->lk);
}

/*
    进程主动放弃CPU控制权
    功能：RUNNING → RUNNABLE，触发切换到调度器
*/
void proc_yield()
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;

    if (p == NULL) {
        return;
    }

    // 原子修改状态为RUNNABLE
    spinlock_acquire(&p->lk);
    if (p->state == RUNNING) {
        p->state = RUNNABLE;
    }
    spinlock_release(&p->lk);

    // 切换到调度器
    proc_sched();
}

/* 
    用户进程切换到调度器
    前提：调用者已持有当前进程的锁
*/
void proc_sched()
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;

    // 断言检查：确保当前进程状态合法
    assert(p != NULL, "proc_sched: no running proc");
    assert(spinlock_holding(&p->lk), "proc_sched: not holding proc lock");
    assert(p->state != RUNNING, "proc_sched: proc is still running");

    // 解除CPU与进程的绑定
    c->proc = NULL;

    // 关键：切换到调度器（原生进程）
    swtch(&p->ctx, (context_t*)__builtin_frame_address(0));

    // 切换回来后：重新绑定CPU与进程
    c->proc = p;
}

/* 
    调度器主逻辑（原生进程执行，永不返回）
    循环扫描进程数组，选择RUNNABLE进程切换执行
*/
void proc_scheduler()
{
    cpu_t *c = mycpu();
    proc_t *p;

    // 调度器死循环
    while (1) {
        intr_off(); // 禁用中断，避免扫描被打断

        // 循环扫描进程数组（按PID顺序，循环调度）
        for (int i = 0; i < N_PROC; i++) {
            p = &proc_list[i];
            spinlock_acquire(&p->lk);

            // 找到RUNNABLE状态的进程
            if (p->state == RUNNABLE) {
                // 标记为RUNNING，占用CPU
                p->state = RUNNING;
                // 绑定CPU与进程
                c->proc = p;
                // 解锁进程锁
                spinlock_release(&p->lk);

                // 切换页表到用户进程的私有页表
                w_satp(MAKE_SATP(p->pgtbl));
                sfence_vma(); // 刷新TLB，确保地址翻译正确

                // 关键：切换到用户进程
                swtch((context_t*)__builtin_frame_address(0), &p->ctx);

                // 切换回来后：恢复内核页表
                w_satp(MAKE_SATP(kernel_pgtbl));
                sfence_vma();

                // 解除CPU与进程的绑定
                c->proc = NULL;

                // 找到一个进程后重新扫描，实现循环调度
                break;
            }

            spinlock_release(&p->lk);
        }

        intr_on(); // 启用中断，允许时钟抢占
    }
}