#include "mod.h"
#include "../mem/mod.h"
#include "../lib/mod.h"
#include "../../user/initcode.h"
#define initcode target_user_initcode
#define initcode_len target_user_initcode_len

// in trampoline.S
extern char trampoline[];

// in swtch.S
extern void swtch(context_t *old, context_t *new);

// in trap/trap_user.c
extern void trap_user_return();

/* ------------本地变量----------- */

// 进程结构体数组 + 第一个用户进程的指针
static proc_t proc_list[N_PROC];
static proc_t *proczero;

// 全局pid + 保护它的锁
static int global_pid;
static spinlock_t pid_lk;

// wait/exit同步锁
static spinlock_t wait_lk;

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
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];
        spinlock_acquire(&p->lk);
        if (p->state == UNUSED) {          
            // 1. 分配PID（移除硬编码，统一检查有效性）
            p->pid = alloc_pid();

            // 2. 分配并初始化trapframe（异常/中断上下文）
            p->tf = (trapframe_t *)pmem_alloc(true);
            if (p->tf == NULL) {
                p->state = UNUSED;
                spinlock_release(&p->lk);
                return NULL;
            }
            memset(p->tf, 0,PGSIZE);


            // 3. 创建用户页表（关联trapframe映射）
            p->pgtbl = proc_pgtbl_init((uint64)p->tf);
            if (p->pgtbl == NULL) {
                pmem_free((uint64)p->tf, true);
                p->tf = NULL;
                p->state = UNUSED;
                spinlock_release(&p->lk);
                return NULL;
            };

            // 4. 初始化基础字段（正确清零，替换错误的uvm_copyin）
            memset(p->name, 0, sizeof(p->name));
            p->parent = NULL;
            p->exit_code = 0;
            p->sleep_space = NULL;
            p->mmap = NULL;
            p->heap_top = 0;
            p->ustack_npage = 0;

            // 5. 初始化内核栈（计算合法地址，避免0值）
            p->kstack = KSTACK(p - proc_list);

            // 6. 初始化内核上下文（补全sp栈顶，修复原代码缺失）
            memset(&p->ctx, 0, sizeof(context_t));
            p->ctx.sp = p->kstack + PGSIZE;      // 内核栈顶（向下生长）
            p->ctx.ra = (uint64)proc_return;     // 进程返回入口

            // 7. 所有资源初始化完成后，再设置为RUNNABLE（核心修复）
            p->state = RUNNABLE;
            return p;
        }
        spinlock_release(&p->lk);
    }
    return NULL;
}

/* 
    回收一个进程结构体并释放它包含的资源
    tips: 调用者需要持有进程锁
*/
/* 
    回收一个进程结构体并释放它包含的资源
    tips: 调用者需要持有进程锁
*/
/* 
    申请一个UNUSED进程结构体(返回时带锁)
    并执行通用的初始化逻辑
*/
void proc_free(proc_t *p)
{
    // 前置断言：保证输入合法性（调用者必须持锁）
    assert(p != NULL, "proc_free: NULL pointer");
    assert(spinlock_holding(&p->lk), "proc_free: not holding proc lock");
    assert(p->state == ZOMBIE || p->state == UNUSED, "proc_free: invalid state");

    // 临时变量：缓存可分配物理内存范围（避免重复计算）
    const uint64 phys_begin = (uint64)ALLOC_BEGIN;
    const uint64 phys_end = (uint64)ALLOC_END;

    // 释放用户页表及其管理的所有物理页（基于vm_unmappages简化实现）
    if (p->pgtbl != NULL) {
        // 1. 释放用户栈（复用vm_unmappages，自动处理页表遍历/校验/释放）
        if (p->ustack_npage > 0) {
            uint64 ustack_bottom = TRAPFRAME - p->ustack_npage * PGSIZE;
            vm_unmappages(p->pgtbl, ustack_bottom, p->ustack_npage * PGSIZE, true);
        }

        // 2. 释放堆空间（复用vm_unmappages，自动计算范围+释放）
        if (p->heap_top > USER_BASE) {
            uint64 heap_size = p->heap_top - USER_BASE;
            vm_unmappages(p->pgtbl, USER_BASE, heap_size, true);
        }

        // 3. 释放所有mmap区域（复用vm_unmappages简化遍历逻辑）
        mmap_region_t *mmap = p->mmap;
        while (mmap != NULL) {
            mmap_region_t *next = mmap->next; // 先保存下一个节点，避免释放当前节点后丢失链表
            // 释放mmap映射的物理页（vm_unmappages自动处理合法性校验）
            if (mmap->begin != 0 && mmap->npages > 0) {
                vm_unmappages(p->pgtbl, mmap->begin, mmap->npages * PGSIZE, true);
            }
            // 释放mmap节点本身（保留地址合法性校验）
            uint64 mmap_node_pa = (uint64)mmap;
            if (mmap_node_pa % PGSIZE == 0 && mmap_node_pa >= phys_begin && mmap_node_pa < phys_end) {
                pmem_free(mmap_node_pa, true);
            }
            mmap = next;
        }
        p->mmap = NULL; // 清空链表头，避免悬空指针

        // 4. 解除trapframe和trampoline映射（不释放物理页，复用vm_unmappages）
        vm_unmappages(p->pgtbl, TRAPFRAME, PGSIZE, false);
        vm_unmappages(p->pgtbl, TRAMPOLINE, PGSIZE, false);

        // 5. 释放页表本身占用的物理页（保留核心校验）
        uint64 pgtbl_pa = (uint64)p->pgtbl;
        if (pgtbl_pa % PGSIZE == 0 && pgtbl_pa >= phys_begin && pgtbl_pa < phys_end) {
            uvm_destroy_pgtbl(p->pgtbl); // 销毁页表结构，释放页表层级资源
            pmem_free(pgtbl_pa, true);   // 释放页表占用的物理内存页
        }
        p->pgtbl = NULL; // 清空页表指针，避免野指针访问
    }

    // 释放trapframe（保留核心地址合法性校验）
    if (p->tf != NULL) {
        uint64 tf_pa = (uint64)p->tf;
        if (tf_pa % PGSIZE == 0 && tf_pa >= phys_begin && tf_pa < phys_end) {
            pmem_free(tf_pa, true); // 释放trapframe占用的物理页
        }
        p->tf = NULL; // 清空trapframe指针，避免野指针
    }

    // 清空其他字段（保持原有逻辑）
    p->pid = 0;                  // 重置进程ID
    p->parent = NULL;            // 解除父进程关联
    p->exit_code = 0;            // 清空退出码
    p->sleep_space = NULL;       // 清空睡眠等待空间
    p->heap_top = 0;             // 重置堆顶地址
    p->ustack_npage = 0;         // 重置用户栈页数
    p->kstack = 0;               // 重置内核栈地址
    memset(p->name, 0, sizeof(p->name));       // 进程名清零
    memset(&p->ctx, 0, sizeof(context_t));     // 执行上下文清零
    
    // 状态设置为UNUSED（标记进程结构体可复用）
    p->state = UNUSED;
    
    // 释放进程锁（调用者持有的锁在此处释放，恢复并发访问）
    spinlock_release(&p->lk);
}


/* 
    获得一个初始化过的用户页表
    完成trapframe和trampoline的映射
*/
pgtbl_t proc_pgtbl_init(uint64 trapframe_pa)
{
    pgtbl_t pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(pgtbl != NULL, "proc_pgtbl_init: root pgtbl alloc failed");
    memset(pgtbl, 0, PGSIZE);

    vm_mappages(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    vm_mappages(pgtbl, TRAPFRAME, trapframe_pa, PGSIZE, PTE_R | PTE_W);

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
*/
void proc_make_first()
{
    // 用户栈地址定义（替换原有USTACK_TOP/USTACK_BOT）
    #define USTACK (TRAPFRAME - PGSIZE)

    // 使用 proc_alloc 申请第一个进程
    proczero = proc_alloc();
    assert(proczero != NULL, "proc_make_first: proczero alloc failed");
    proc_t *p = proczero;

    // 设置进程名称
    const char *proc_name = "proczero";
    uint32 name_max_len = sizeof(p->name) - 1; // 预留终止符空间
    uint32 name_len = 0;

    // 手动遍历计算字符串长度（兼容无 strlen 的极简环境）
    while (proc_name[name_len] != '\0' && name_len < name_max_len) {
        name_len++;
    }

    // 用 memmove 安全拷贝进程名（memmove 兼容内存重叠，更安全）
    if (name_len > 0) {
        memmove(p->name, (void*)proc_name, name_len);
    }
    // 强制添加字符串终止符，避免垃圾数据
    p->name[name_len] = '\0';
    // 剩余空间清零（避免残留数据泄露）
    if (name_len < name_max_len) {
        memset(p->name + name_len + 1, 0, name_max_len - name_len);
    }

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

    // 填写 trapframe 关键字段
    p->tf->user_to_kern_satp = r_satp();
    // 对齐参考版的内核栈地址计算方式
    int proc_id = p - proc_list;
    p->tf->user_to_kern_sp = KSTACK(proc_id) + PGSIZE;  // 内核栈顶
    extern void trap_user_handler(); // 补充参考版的extern声明
    p->tf->user_to_kern_trapvector = (uint64)trap_user_handler;
    p->tf->user_to_kern_epc = USER_BASE; // 用户态入口（initcode起始）
    p->tf->user_to_kern_hartid = r_tp();  // 当前核ID
    p->tf->sp = USTACK + PGSIZE; // 改用USTACK宏计算用户栈顶

    // 状态设置为 RUNNABLE（等待调度器调度）
    p->state = RUNNABLE;//

    // 释放进程锁(proc_alloc 返回时持有锁)
    spinlock_release(&p->lk);
}

/*
    父进程产生子进程
    UNUSED -> RUNNABLE
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
    // ===== 替代uvm_copyin：手动逐字符拷贝进程名称（极简版）=====
    uint32 name_max_len = sizeof(child->name) - 1;
    uint32 i = 0;
    // 逐字符拷贝，直到父进程名结束或达到最大长度
    while (i < name_max_len && parent->name[i] != '\0') {
        child->name[i] = parent->name[i];
        i++;
    }
    child->name[i] = '\0'; // 强制添加字符串终止符

    // ===== 复制用户态页表（复用原有页表初始化逻辑）=====
    child->pgtbl = proc_pgtbl_init((uint64)child->tf);
    uvm_copy_pgtbl(parent->pgtbl, child->pgtbl, parent->heap_top, parent->ustack_npage, parent->mmap);

    // ===== 手动拷贝堆、栈、mmap信息（逐字段赋值）=====
    child->heap_top = parent->heap_top;
    child->ustack_npage = parent->ustack_npage;
    child->mmap = parent->mmap; // 简化：浅拷贝，后续可扩展为链表逐节点拷贝

    // ===== 替代uvm_copyin：手动逐字段拷贝陷阱帧（核心字段）=====
    if (parent->tf != NULL && child->tf != NULL) {
    *child->tf = *parent->tf;
    }

    // ===== 替代memcpy：手动处理内核栈（仅初始化栈顶，简化实现）=====
    child->kstack = KSTACK(child->pid); // 分配独立内核栈
    // 极简版：仅拷贝内核上下文核心字段（栈顶偏移），无需拷贝整个栈内容
    child->ctx = parent->ctx;
    child->ctx.sp = child->kstack + (parent->ctx.sp - parent->kstack); // 保持栈偏移一致

    // 3. 建立父子关系
    child->parent = parent;
    // 核心修正：将陷阱帧中内核栈指针从父进程地址替换为子进程独立内核栈的栈顶
    // PGSIZE是页大小（如4096），child->kstack是内核栈基址，+PGSIZE指向栈顶（RISC-V栈向下生长）
    child->tf->user_to_kern_sp = child->kstack + PGSIZE;
    // 4. 解锁子进程，设为可运行（proc_alloc默认初始化state为RUNNABLE）
    spinlock_release(&child->lk);

    // 父进程返回子进程PID
    return child->pid;
}
/*
    进程主动放弃CPU控制权
    RUNNING->RUNNABLE
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
    // 切换到调度器
    proc_sched();
    spinlock_release(&p->lk);
}

/*
    唤醒等待呼叫的进程
    由proc_exit调用
    tips: 调用者需要持有p的进程锁
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
    当父进程退出时, 让它的所有子进程认proczero为父
    因为proczero永不退出, 可以回收子进程的资源
*/
static void proc_reparent(proc_t *parent)
{
    // 入参校验：父进程不能为空
    if (parent == NULL) {
        return;
    }

    // 2. 遍历所有进程（假设proc_list是进程数组，NPROC是最大进程数）
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];

        // 跳过无效进程（未初始化/已释放）
        if (p->state == UNUSED) {
            continue;
        }

        // 加当前进程的锁（避免修改进程时被其他逻辑干扰）
        spinlock_acquire(&p->lk);

        // 3. 找到以当前parent为父的子进程
        if (p->parent == parent) {
            // 将子进程的父进程改为proczero（零号进程）
            p->parent = proczero;
            
            // 如果子进程已经是ZOMBIE状态,需要唤醒proczero来回收
            if (p->state == ZOMBIE) {
                proc_try_wakeup(p);
            }
        }

        // 释放当前进程的锁
        spinlock_release(&p->lk);
    }
}
/*
    进程退出
    RUNNING -> ZOMBIE
*/
void proc_exit(int exit_code)
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    assert(p != NULL && p->state == RUNNING, "proc_exit: not running");
    proc_reparent(p);
    spinlock_acquire(&p->lk);

    // 1. 设置进程状态为ZOMBIE，保存退出码
    p->state = ZOMBIE;
    p->exit_code = exit_code;

    // 2. 唤醒父进程（父进程可能在proc_wait中睡眠）
    proc_try_wakeup(p);

    proc_sched();

    // 永远不会执行到这里
    panic("proc_exit: should not return");
}

/*
    父进程等待一个子进程进入ZOMBIE状态
    1. 如果等到: 释放子进程, 返回子进程的pid, 将子进程的退出状态传出到user_addr
    2. 如果发现没孩子: 返回-1
    3. 如果没等到: 父进程进入睡眠状态 
*/
int proc_wait(uint64 user_addr)
{
    cpu_t *c = mycpu();
    proc_t *parent = c->proc;
    assert(parent != NULL && parent->state == RUNNING, "proc_wait: parent not running");
    spinlock_acquire(&wait_lk);
    while (1) {
        int has_children = 0; // 是否有子进程
        // 1. 扫描所有进程，查找当前进程的ZOMBIE态子进程
        for (int i = 0; i < N_PROC; i++) {
            proc_t *child = &proc_list[i];
            spinlock_acquire(&child->lk);

            if (child->parent == parent) {
                has_children = 1;
                // 找到一个ZOMBIE状态的子进程
                if (child->state == ZOMBIE) {
                // 找到目标子进程，记录PID和退出码
                int pid = child->pid;
                int exit_code = child->exit_code;

                // 2. 回收子进程资源
                proc_free(child);

                // 3. 将退出码写入用户态地址（用uvm_copyout确保安全）
                if (user_addr != 0) {
                    uvm_copyout(parent->pgtbl, user_addr, (uint64)&exit_code, sizeof(int));
                }
                    spinlock_release(&wait_lk);
                return pid; // 返回子进程PID
                }
            }

            spinlock_release(&child->lk);
        }
        // 如果没有子进程,返回-1
        if (!has_children) {
            spinlock_release(&wait_lk);
            return -1;
        }
        // 2. 未找到ZOMBIE子进程，进入睡眠态（等待子进程退出唤醒）
        proc_sleep(parent, &wait_lk);// 睡眠资源设为自身，父进程等待被唤醒
    }
}
/*
    进程等待sleep_space对应的资源, 进入睡眠状态
    RUNNING -> SLEEPING
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;
    assert(p != NULL && p->state == RUNNING, "proc_sleep: not running");
    assert(spinlock_holding(lock), "proc_sleep: not holding lock");

    spinlock_acquire(&p->lk);
    spinlock_release(lock);
    // 1. 绑定睡眠资源，设置状态为SLEEPING
    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    // 3. 切换到调度器（睡眠进程不再占用CPU）

    proc_sched();
    p->sleep_space = NULL;
    spinlock_release(&p->lk);
    // 4. 被唤醒后，重新获取传入的锁（恢复睡眠前的锁状态）
    spinlock_acquire(lock);
}

/*
    唤醒所有等待sleep_space的进程
    SLEEPING -> RUNNABLE
*/
void proc_wakeup(void *sleep_space)
{
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
}

/* 
    用户进程切换到调度器
    tips: 调用者保证持有当前进程的锁
*/
/* 
    用户进程切换到调度器
    tips: 调用者保证持有当前进程的锁
*/
void proc_sched()
{
    cpu_t *c = mycpu(); // 修复点1：提前获取CPU结构体，避免多次调用mycpu()
    proc_t *p = c->proc; // 修复点2：从CPU获取进程，而非myproc()，避免上下文不一致
    
    // 修复点3：调整断言顺序，先校验p非空，再校验锁
    assert(p != NULL, "proc_sched: no process running");
    assert(spinlock_holding(&p->lk), "proc_sched: not holding lock");
    assert(p->state != RUNNING, "proc_sched: process still in RUNNING state"); // 新增：确保进程状态已切换
    
    // 修复点6：校验调度器上下文合法性（核心：避免跳转到非法地址）
    assert(c->ctx.sp != 0 && c->ctx.ra != 0, "proc_sched: invalid scheduler ctx");
    
    // 修复点7：校验进程上下文合法性（避免切换后sepc非法）
    assert(p->ctx.ra != 0 && p->ctx.sp != 0, "proc_sched: invalid process ctx");
    // assert(p->ctx.sp >= p->kstack && p->ctx.sp < p->kstack + PGSIZE, 
    //        "proc_sched: process sp out of kstack");
    
    // 保存当前进程的上下文,切换到调度器的上下文
    swtch(&p->ctx, &c->ctx);
}

/* 
    调度器
    RUNNABLE->RUNNING
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
                //printf("proc %d is running...\n", p->pid);
                swtch(&c->ctx, &p->ctx);
            }
            spinlock_release(&p->lk);
        }
        intr_on(); // 启用中断，允许时钟抢占
    }
}