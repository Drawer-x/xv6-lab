#include "mod.h"
#include "method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../lib/method.h"
#include "../lib/type.h"
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len
// 外部符号声明
extern void swtch(context_t *old_ctx, context_t *new_ctx);
extern void intr_off();
extern void intr_on();
extern cpu_t *mycpu();
extern pgtbl_t kernel_pgtbl;
extern char trampoline[];
extern void user_return(trapframe_t *tf, uint64 satp);
extern void trap_user_handler();
extern void trap_user_return();

// 汇编辅助宏（补充完整）
#ifndef r_tp
#define r_tp() ({ uint64 x; asm volatile("mv %0, tp" : "=r"(x)); x; })
#endif

#ifndef w_satp
#define w_satp(x) asm volatile("csrw satp, %0" : : "r"(x))
#endif
                            
#ifndef sfence_vma
#define sfence_vma() asm volatile("sfence.vma" : : : "memory")
#endif

#ifndef r_satp
#define r_satp() ({ uint64 x; asm volatile("csrr %0, satp" : "=r"(x)); x; })
#endif

/* ------------本地变量----------- */
static proc_t proc_list[N_PROC];
static proc_t *proczero;
static int global_pid;
static spinlock_t pid_lk;
static spinlock_t wait_lk; // 恢复wait_lk，解决wait/exit竞态

/* 提前声明静态函数 */
static void proc_try_wakeup(proc_t *p);
static int alloc_pid();
static void proc_return();

/* 获取一个pid */
static int alloc_pid()
{
    int tmp = 0;
    spinlock_acquire(&pid_lk);
    // 修正：PID范围检查（1~N_PROC）
    assert(global_pid > 0 && global_pid <= N_PROC, "alloc_pid: overflow");
    tmp = global_pid++;
    spinlock_release(&pid_lk);
    return tmp;
}

/* 释放进程锁 + trap_user_return */
static void proc_return()
{
    spinlock_release(&myproc()->lk);
    trap_user_return();
}

/* 进程模块初始化 */
void proc_init()
{
    spinlock_init(&pid_lk, "pid_lock");
    spinlock_init(&wait_lk, "wait_lock"); // 初始化wait_lk
    global_pid = 1;

    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        p->state = UNUSED;
        spinlock_init(&p->lk, "proc_lock");
        
        // 正确使用memset（适配内核定义）
        memset(p->name, (uint8)0, (uint32)sizeof(p->name));
        
        p->parent = NULL;
        p->exit_code = 0;
        p->sleep_space = NULL;
        p->pgtbl = NULL;
        p->heap_top = 0;
        p->ustack_npage = 0;
        p->mmap = NULL;
        p->tf = NULL;
        p->kstack = 0;
        
        memset(&p->ctx, (uint8)0, (uint32)sizeof(context_t));
    }
}

/* 
    申请一个UNUSED进程结构体(返回时带锁)
    并执行通用的初始化逻辑
*/
proc_t *proc_alloc()
{
    proc_t *p = NULL;
    
    // 扫描进程数组，寻找第一个 UNUSED 的进程
    for (int i = 0; i < N_PROC; i++) {
        spinlock_acquire(&proc_list[i].lk);
        if (proc_list[i].state == UNUSED) {
            p = &proc_list[i];
            break;
        }
        spinlock_release(&proc_list[i].lk);
    }
    
    if (p == NULL) {
        return NULL;  // 没有可用进程
    }
    
    // 分配 PID
    p->pid = alloc_pid();
    
    // 分配 trapframe
    p->tf = (trapframe_t *)pmem_alloc(true);
    if (p->tf == NULL) {
        p->state = UNUSED;
        spinlock_release(&p->lk);
        return NULL;
    }
    memset(p->tf, 0, PGSIZE);
    
    // 创建用户页表（包括 trapframe 和 trampoline 映射）
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);
    if (p->pgtbl == NULL) {
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
        p->state = UNUSED;
        spinlock_release(&p->lk);
        return NULL;
    }
    
    // 初始化其他字段
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->mmap = NULL;
    memset(p->name, 0, sizeof(p->name));
    
    // 计算进程在数组中的索引，设置内核栈
    int proc_id = p - proc_list;
    p->kstack = KSTACK(proc_id);
    
    // 初始化内核上下文
    memset(&p->ctx, 0, sizeof(p->ctx));
    p->ctx.sp = p->kstack + PGSIZE;      // 内核栈顶
    p->ctx.ra = (uint64)proc_return;     // 返回地址设为 proc_return
    
    // 状态设置为 RUNNABLE
    p->state = RUNNABLE;
    
    // 返回时持有进程锁
    return p;
}
/* 回收一个进程结构体并释放它包含的资源 */
void proc_free(proc_t *p)
{
    // 释放 trapframe
    if (p->tf) {
        pmem_free((uint64)p->tf, true);
        p->tf = NULL;
    }
    
    // 释放用户页表及其管理的所有物理页
    if (p->pgtbl) {
        // 释放用户代码页
        vm_unmappages(p->pgtbl, USER_BASE, PGSIZE, true);
        
        // 释放堆空间
        if (p->heap_top > USER_BASE + PGSIZE) {
            uint64 heap_size = p->heap_top - (USER_BASE + PGSIZE);
            vm_unmappages(p->pgtbl, USER_BASE + PGSIZE, heap_size, true);
        }
        
        // 释放所有 mmap 区域
        mmap_region_t *tmp = p->mmap;
        while (tmp != NULL) {
            vm_unmappages(p->pgtbl, tmp->begin, tmp->npages * PGSIZE, true);
            mmap_region_t *next = tmp->next;
            mmap_region_free(tmp);
            tmp = next;
        }
        p->mmap = NULL;
        
        // 释放用户栈
        if (p->ustack_npage > 0) {
            uint64 ustack_bottom = TRAPFRAME - p->ustack_npage * PGSIZE;
            vm_unmappages(p->pgtbl, ustack_bottom, p->ustack_npage * PGSIZE, true);
        }
        
        // 解除 trapframe 和 trampoline 的映射（不释放物理页）
        vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);
        
        // 释放页表本身占用的物理页
        pmem_free((uint64)p->pgtbl, true);
        p->pgtbl = NULL;
    }
    
    // 清空其他字段
    p->pid = 0;
    p->parent = NULL;
    p->exit_code = 0;
    p->sleep_space = NULL;
    p->heap_top = 0;
    p->ustack_npage = 0;
    p->kstack = 0;
    memset(p->name, 0, sizeof(p->name));
    memset(&p->ctx, 0, sizeof(p->ctx));
    
    // 状态设置为 UNUSED
    p->state = UNUSED;
    
    // 释放进程锁
    spinlock_release(&p->lk);
}

/** 为用户进程创建页表 */
pgtbl_t proc_pgtbl_init(uint64 trapframe_pa)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: root pgtbl alloc failed");
    // 正确使用memset初始化页表
    memset(pgtbl, (uint8)0, (uint32)PGSIZE);

    // 映射trampoline和trapframe
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

    // 设置进程名称（正确拷贝）
    const char *name_src = "proczero";
    uint32 name_len = sizeof(p->name) - 1;
    for (uint32 i = 0; i < name_len && name_src[i]; i++) {
        p->name[i] = name_src[i];
    }
    p->name[sizeof(p->name)-1] = '\0';
    
    // 手动设置proczero的PID为0
    spinlock_acquire(&pid_lk);
    p->pid = 0;
    spinlock_release(&pid_lk);

    // 初始化页表
    p->pgtbl = proc_pgtbl_init((uint64)p->tf);

    // 分配用户代码页
    void *ucode_pa = pmem_alloc(false);
    assert(ucode_pa != NULL, "proc_make_first: initcode page alloc failed");
    memset(ucode_pa, (uint8)0, (uint32)PGSIZE);
    // 拷贝初始化代码
    uint32 copy_len = (target_user_initcode_len > PGSIZE) ? PGSIZE : target_user_initcode_len;
    memmove(ucode_pa, initcode, copy_len);
    vm_mappages(p->pgtbl, USER_BASE, (uint64)ucode_pa, PGSIZE, PTE_R | PTE_X | PTE_U);

    // 分配用户栈
    const uint64 USTACK_TOP = USER_BASE + 8 * PGSIZE;
    const uint64 USTACK_BOT = USTACK_TOP - PGSIZE;
    void *ustack_pa = pmem_alloc(false);
    assert(ustack_pa != NULL, "proc_make_first: ustack page alloc failed");
    memset(ustack_pa, (uint8)0, (uint32)PGSIZE);
    vm_mappages(p->pgtbl, USTACK_BOT, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_npage = 1;
    p->heap_top = USTACK_BOT;

    // 初始化内核栈
    p->kstack = KSTACK(0);
    p->ctx.sp = p->kstack + PGSIZE;
    p->ctx.ra = (uint64)proc_return; // 正确指向proc_return

    // 初始化陷阱帧
    p->tf->user_to_kern_satp = MAKE_SATP(kernel_pgtbl);
    p->tf->user_to_kern_sp = p->kstack + PGSIZE;
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_epc = USER_BASE;
    p->tf->user_to_kern_hartid = r_tp();
    p->tf->sp = USTACK_TOP;

    // 绑定CPU
    cpu_t *c = mycpu();
    c->proc = p;

    printf("[proc] switch to user: epc=%p, usp=%p, ksp=%p\n",
           (void*)USER_BASE, (void*)USTACK_TOP, (void*)(p->kstack + PGSIZE));

    spinlock_release(&p->lk);
}

/* 进程创建（fork） */
int proc_fork()
{
    cpu_t *c = mycpu();
    proc_t *parent = c->proc;
    assert(parent != NULL && parent->state == RUNNING, "proc_fork: parent not running");

    // 申请子进程
    proc_t *child = proc_alloc();
    if (child == NULL) {
        return -1;
    }

    // 复制进程名称
    uint32 name_len = sizeof(child->name) - 1;
    for (uint32 i = 0; i < name_len && parent->name[i]; i++) {
        child->name[i] = parent->name[i];
    }
    child->name[name_len] = '\0';

    // 复制页表
    child->pgtbl = proc_pgtbl_init((uint64)child->tf);
    uvm_copy_pgtbl(parent->pgtbl, child->pgtbl, parent->heap_top, parent->ustack_npage, parent->mmap);

    // 复制堆、栈、mmap信息
    child->heap_top = parent->heap_top;
    child->ustack_npage = parent->ustack_npage;
    child->mmap = parent->mmap; // 简化实现，实际需深拷贝

    // 复制陷阱帧
    if (parent->tf != NULL && child->tf != NULL) {
        memmove(child->tf, parent->tf, sizeof(trapframe_t));
        child->tf->a0 = 0; // 子进程返回0
        child->tf->user_to_kern_sp = child->kstack + PGSIZE;
    }

    // 初始化内核栈
    child->kstack = KSTACK(child->pid % N_PROC); // 防止越界
    child->ctx = parent->ctx;
    child->ctx.sp = child->kstack + (parent->ctx.sp - parent->kstack);
    child->ctx.ra = (uint64)proc_return;

    // 建立父子关系
    child->parent = parent;

    // 解锁子进程
    spinlock_release(&child->lk);

    return child->pid;
}

/* 进程退出 */
void proc_exit(int exit_code)
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    assert(p != NULL && p->state == RUNNING, "proc_exit: not running");
    assert(p != proczero, "proc_exit: proczero cannot exit");

    // 持有wait_lk，保证过继子进程原子性
    spinlock_acquire(&wait_lk);
    spinlock_acquire(&p->lk);

    // 设置退出状态
    p->state = ZOMBIE;
    p->exit_code = exit_code;

    // 过继子进程给proczero
    for (int i = 0; i < N_PROC; i++) {
        proc_t *child = &proc_list[i];
        spinlock_acquire(&child->lk);
        if (child->state != UNUSED && child->parent == p) {
            child->parent = proczero;
            // 唤醒proczero处理僵尸子进程
            if (child->state == ZOMBIE) {
                proc_try_wakeup(child);
            }
        }
        spinlock_release(&child->lk);
    }

    // 唤醒父进程
    proc_try_wakeup(p);
    spinlock_release(&wait_lk);

    // 切换到调度器
    c->proc = NULL;
    spinlock_release(&p->lk);
    proc_sched();

    panic("proc_exit: should not return");
}

/* 进程等待 */
int proc_wait(uint64 user_addr)
{
    cpu_t *c = mycpu();
    proc_t *parent = c->proc;
    assert(parent != NULL && parent->state == RUNNING, "proc_wait: parent not running");

    spinlock_acquire(&wait_lk); // 持有全局锁
    while (1) {
        intr_off();
        int has_children = 0;
        int found = 0;
        int pid = -1;
        int exit_code = 0;

        // 扫描子进程
        for (int i = 0; i < N_PROC; i++) {
            proc_t *child = &proc_list[i];
            spinlock_acquire(&child->lk);

            if (child->parent == parent) {
                has_children = 1;
                if (child->state == ZOMBIE) {
                    // 记录子进程信息
                    pid = child->pid;
                    exit_code = child->exit_code;
                    // 回收子进程
                    proc_free(child);
                    found = 1;
                }
            }

            spinlock_release(&child->lk);
            if (found) break;
        }

        // 找到僵尸子进程
        if (found) {
            spinlock_release(&wait_lk);
            intr_on();
            // 写入退出码到用户空间
            if (user_addr != 0) {
                uvm_copyout(parent->pgtbl, user_addr, (uint64)&exit_code, sizeof(int));
            }
            return pid;
        }

        // 无子女
        if (!has_children) {
            spinlock_release(&wait_lk);
            intr_on();
            return -1;
        }

        // 睡眠等待
        proc_sleep(parent, &wait_lk);
        intr_on();
    }
}

/* 进程睡眠 */
void proc_sleep(void *sleep_space, spinlock_t *lock)
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    assert(p != NULL && p->state == RUNNING, "proc_sleep: not running");
    assert(spinlock_holding(lock), "proc_sleep: not holding lock");

    // 防止死锁：先加进程锁，再释放外部锁
    spinlock_acquire(&p->lk);
    spinlock_release(lock);

    // 设置睡眠状态
    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    // 切换到调度器
    c->proc = NULL;
    spinlock_release(&p->lk);
    proc_sched();

    // 唤醒后重新获取外部锁
    spinlock_acquire(lock);
    p->sleep_space = NULL;
}

/* 唤醒进程 */
void proc_wakeup(void *sleep_space)
{
    intr_off();

    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        spinlock_acquire(&p->lk);

        if (p->state == SLEEPING && p->sleep_space == sleep_space) {
            p->state = RUNNABLE;
            p->sleep_space = NULL;
        }

        spinlock_release(&p->lk);
    }

    intr_on();
}

/* 唤醒父进程 */
static void proc_try_wakeup(proc_t *p)
{
    assert(p != NULL, "proc_try_wakeup: invalid proc");

    proc_t *parent = p->parent ?: proczero;
    spinlock_acquire(&parent->lk);

    if (parent->state == SLEEPING && parent->sleep_space == parent) {
        parent->state = RUNNABLE;
        parent->sleep_space = NULL;
        printf("proc %d wakeup parent %d\n", p->pid, parent->pid);
    }

    spinlock_release(&parent->lk);
}

/* 进程主动放弃CPU */
void proc_yield()
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    if (p == NULL) return;

    spinlock_acquire(&p->lk);
    if (p->state == RUNNING) {
        p->state = RUNNABLE;
    }
    spinlock_release(&p->lk);

    proc_sched();
}

/* 进程切换到调度器 */
void proc_sched()
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;

    assert(p != NULL, "proc_sched: no running proc");
    assert(spinlock_holding(&p->lk), "proc_sched: not holding proc lock");
    assert(p->state != RUNNING, "proc_sched: proc is still running");

    // 切换上下文：保存进程上下文，恢复调度器上下文
    c->proc = NULL;
    swtch(&p->ctx, &c->ctx);
    c->proc = p;
}

/* 极简版调度器：修复锁加解锁不匹配问题 */
void proc_scheduler() {
    cpu_t *c = mycpu();
    if (c == NULL) panic("proc_scheduler: mycpu() return NULL");

    while (1) {
        int found = 0;
        proc_t *target_proc = NULL;

        for (int idx = 0; idx < N_PROC; idx++) {
            if (idx < 0 || idx >= N_PROC) break;
            proc_t *p = &proc_list[idx];
            
            // 加锁前先检查进程地址合法性（避免访问 0x0 进程）
            if ((unsigned long)p == 0) {
                continue;
            }

            // 1. 加锁（必须先加锁，再操作进程）
            spinlock_acquire(&p->lk);

            // 2. 检查进程状态
            if (p->state == RUNNABLE) {
                target_proc = p;
                found = 1;
                // 3. 找到进程后，先解锁再break（避免锁持有状态退出）
                spinlock_release(&p->lk);
                break;
            }

            // 4. 未找到进程，正常解锁
            spinlock_release(&p->lk);
        }

        if (found && target_proc != NULL) {
            // 操作目标进程前，重新加锁（关键：和上面的解锁配对）
            spinlock_acquire(&target_proc->lk);
            
            intr_off();
            target_proc->state = RUNNING;
            c->proc = target_proc;
            // 5. 状态修改完成，立即解锁（不要持有锁切换上下文）
            spinlock_release(&target_proc->lk);

            // 上下文切换（调度器 → 用户进程）
            swtch(&c->ctx, &target_proc->ctx);

            // 进程切回调度器，重新加锁处理状态
            spinlock_acquire(&target_proc->lk);
            if (target_proc->state == ZOMBIE) {
                proc_free(target_proc);
                target_proc->state = UNUSED;
            } else if (target_proc->state != UNUSED) {
                target_proc->state = RUNNABLE;
            }
            // 6. 状态处理完成，解锁
            spinlock_release(&target_proc->lk);

            c->proc = NULL;
            intr_on();
        } else {
            intr_on();
            asm volatile("wfi");
        }
    }
}
