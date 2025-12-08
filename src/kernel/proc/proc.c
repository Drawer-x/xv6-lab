#include "mod.h"
#include "../mem/mod.h"
#include "../lib/mod.h"
#include "../../user/initcode.h"
#include "../fs/method.h"   // fs_init()

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
void proc_return()
{
    static int fs_inited = 0;

    // proczero 第一次返回用户态之前，做文件系统初始化（先置位再初始化，防止并发/重入）
    if (!fs_inited) {
        fs_inited = 1;
        fs_init();
    }

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
    // 用户栈底地址（紧贴 TRAPFRAME 之下）
    #define USTACK (TRAPFRAME - PGSIZE)

    // 申请 proczero
    proczero = proc_alloc();
    assert(proczero != NULL, "proc_make_first: proczero alloc failed");
    proc_t *p = proczero;

    // 设置进程名称
    const char *proc_name = "proczero";
    uint32 name_max_len = sizeof(p->name) - 1;
    uint32 name_len = 0;
    while (proc_name[name_len] != '\0' && name_len < name_max_len) name_len++;
    if (name_len > 0) {
        memmove(p->name, (void*)proc_name, name_len);
    }
    p->name[name_len] = '\0';
    if (name_len < name_max_len) {
        memset(p->name + name_len + 1, 0, name_max_len - name_len);
    }

    // 分配并映射用户 code 页到 USER_BASE
    void *ucode_pa = pmem_alloc(false);
    assert(ucode_pa != NULL && (uint64)ucode_pa != 0, "proc_make_first: initcode page alloc failed");
    memset(ucode_pa, 0, PGSIZE);

    uint32 copy_len = (target_user_initcode_len > PGSIZE) ? PGSIZE : target_user_initcode_len;
    if (copy_len > 0 && target_user_initcode != NULL) {
        memmove(ucode_pa, (void*)target_user_initcode, copy_len);
    }

    assert(p->pgtbl != NULL && USER_BASE != 0 && (uint64)ucode_pa != 0,
           "proc_make_first: ucode map param invalid");
    vm_mappages(p->pgtbl, USER_BASE, (uint64)ucode_pa, PGSIZE, PTE_R | PTE_X | PTE_U);

    // 分配并映射用户栈页到 USTACK
    void *ustack_pa = pmem_alloc(false);
    assert(ustack_pa != NULL && (uint64)ustack_pa != 0, "proc_make_first: ustack page alloc failed");
    memset(ustack_pa, 0, PGSIZE);

    assert(p->pgtbl != NULL && USTACK != 0 && (uint64)ustack_pa != 0,
           "proc_make_first: ustack map param invalid");
    vm_mappages(p->pgtbl, USTACK, (uint64)ustack_pa, PGSIZE, PTE_R | PTE_W | PTE_U);
    p->ustack_npage = 1;
    p->heap_top = USER_BASE + PGSIZE;

    // ===== 关键：按“标准 trapframe 布局”填写 5 个内核字段与用户 EPC =====
    extern void trap_user_handler();
    int proc_id = p - proc_list;

    p->tf->kernel_satp   = r_satp();                         // 返回内核时切回的 satp
    p->tf->kernel_sp     = KSTACK(proc_id) + PGSIZE;         // 内核栈顶
    p->tf->kernel_trap   = (uint64)trap_user_handler;        // S 态 trap 入口（C 处理函数）
    p->tf->epc           = USER_BASE;                        // 用户入口（initcode）
    p->tf->kernel_hartid = r_tp();                           // 当前 hartid
    // 用户态 sp（trampoline.S 会从 trapframe 恢复它）
    p->tf->sp            = USTACK + PGSIZE;

    // 设为可运行，等待调度
    p->state = RUNNABLE;

    // 释放进程锁（proc_alloc 返回时持有）
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

    // 1) 申请子进程结构体（返回时已持有 child->lk）
    proc_t *child = proc_alloc();
    if (child == NULL) {
        return -1;
    }

    // 2) 复制进程名（简化安全拷贝）
    uint32 name_max_len = sizeof(child->name) - 1;
    uint32 i = 0;
    while (i < name_max_len && parent->name[i] != '\0') {
        child->name[i] = parent->name[i];
        i++;
    }
    child->name[i] = '\0';

    // 3) 复制用户地址空间
    //    proc_alloc() 已经为 child 分配了 trapframe 和 pgtbl，
    //    这里只需要把父页表内容 copy 到 child->pgtbl。
    uvm_copy_pgtbl(parent->pgtbl, child->pgtbl,
                   parent->heap_top, parent->ustack_npage, parent->mmap);

    // 4) 复制与用户地址空间相关的元数据
    child->heap_top    = parent->heap_top;
    child->ustack_npage= parent->ustack_npage;
    child->mmap        = parent->mmap;   // 目前保持与你现有逻辑一致（浅拷贝）

    // 5) 复制 trapframe，并修正与“子进程内核栈/当前核”相关的字段
    if (parent->tf != NULL && child->tf != NULL) {
        *child->tf = *parent->tf;
    }

    // 计算子进程的内核栈地址（注意用数组下标而不是 pid）
    child->kstack = KSTACK(child - proc_list);

    // 标准 trapframe 字段修正
    child->tf->kernel_sp     = child->kstack + PGSIZE;  // 子进程返回内核时使用的栈顶
    child->tf->kernel_hartid = r_tp();                  // 当前 hart
    child->tf->kernel_satp   = r_satp();                // 返回内核时应切回的 satp（内核页表）
    // kernel_trap/epc/sp 已被复制自父进程，通常无需改动：
    // - kernel_trap 指向 trap_user_handler（在 proc_make_first 中已设置）
    // - epc 是用户空间返回地址
    // - sp 是用户态栈顶，由 uvm_copy_pgtbl 后保持一致

    // 6) 初始化子进程的内核上下文（不要用整体赋值覆盖）
    memset(&child->ctx, 0, sizeof(context_t));
    child->ctx.sp = child->kstack + PGSIZE;           // 切入子进程时使用的内核栈
    child->ctx.ra = (uint64)proc_return;              // 从调度器切回时落到这里进入用户态

    // 7) 建立父子关系
    child->parent = parent;

    // 8) 解锁并让子进程进入可运行态（proc_alloc 已设 RUNNABLE）
    spinlock_release(&child->lk);

    // 9) 父进程返回子进程 PID
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
    if (p == NULL) return;

    // 约定：切到进程时，p->lk 已经由调度器持有并“交给了进程”。
    // 因此这里不能再 acquire，一旦再 acquire 就会触发 already holding。
    assert(spinlock_holding(&p->lk), "proc_yield: p->lk must be held");

    // 统一保持“中断关闭”的语义，避免在持锁到 sched 之间被时钟中断打断
    push_off();

    if (p->state == RUNNING) {
        p->state = RUNNABLE;
    }

    // 进入调度器：要求当前已持有 p->lk 且中断关闭
    proc_sched();

    // 从调度器切回后，依旧持有 p->lk；现在才由进程侧释放
    spinlock_release(&p->lk);
    pop_off();
}


/*
    唤醒等待呼叫的进程
    由proc_exit调用
    tips: 调用者需要持有p的进程锁
*/
static void proc_try_wakeup(proc_t *p)
{
    //assert(p != NULL && spinlock_holding(&p->lk), "proc_try_wakeup: invalid state");

    // 1. 获取父进程，无父进程则唤醒proczero
    proc_t *parent = p->parent;
    if (parent == NULL) {
        return;
    }

    // 2. 唤醒父进程（父进程可能在proc_wait中睡眠，睡眠资源为自身）
    spinlock_acquire(&parent->lk);
    if (parent->state == SLEEPING && parent->sleep_space == parent) {
        parent->state = RUNNABLE;
        //parent->sleep_space = NULL;
        printf("proc %d is wakeup!\n", parent->pid);
    }
    spinlock_release(&parent->lk);
}
/*
    当父进程退出时, 让它的所有子进程认proczero为父
    因为proczero永不退出, 可以回收子进程的资源
*/
// 当父进程退出时, 让它的所有子进程认 proczero 为父。
// 注意：调用者通常已持有 parent->lk，这里绝不能再给 parent 自己加锁。
static void proc_reparent(proc_t *parent)
{
    if (parent == NULL) return;

    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];

        // 关键修正：跳过 parent 本身，避免对同一把锁二次加锁
        if (p == parent) {
            continue;
        }

        spinlock_acquire(&p->lk);

        if (p->parent == parent) {
            p->parent = proczero;
            if (p->state == ZOMBIE) {
                // 若有僵尸子进程，唤醒 proczero 去回收
                // 这里不会对 parent 加锁，因此无循环依赖
                proc_try_wakeup(p);
            }
        }

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
    //assert(p != NULL && p->state == RUNNING, "proc_exit: not running");
    spinlock_acquire(&wait_lk);
    proc_reparent(p);
    spinlock_acquire(&p->lk);

    // 1. 设置进程状态为ZOMBIE，保存退出码
    p->exit_code = exit_code;
    p->state = ZOMBIE;

    // 2. 唤醒父进程（父进程可能在proc_wait中睡眠）
    proc_try_wakeup(p);
    spinlock_release(&wait_lk);
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
    proc_t *parent = myproc();
    //assert(parent != NULL && parent->state == RUNNING, "proc_wait: parent not running");
    spinlock_acquire(&wait_lk);
    while (1) {
        int has_children = 0; 
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
                printf("proc %d is wakeup!\n", parent->pid);
                // 3. 将退出码写入用户态地址（用uvm_copyout确保安全）
                if (user_addr != 0) {
                    uvm_copyout(parent->pgtbl, user_addr, (uint64)&exit_code, sizeof(int));
                }
                proc_free(child);
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
    让当前进程在 sleep_space 上睡眠。
    语义：
      - 如果调用时未持有 p->lk，这里会临时获取 p->lk，并在返回前释放；
      - 如果调用时已持有 p->lk，这里不重复获取，也不负责释放（保持调用现场不变）。
*/
void proc_sleep(void *sleep_space, spinlock_t *lock)
{
    cpu_t *c = mycpu();
    proc_t *p = c->proc;

    // 统一关中断，避免在“还持有 p->lk 未释放”窗口被时钟中断打断
    push_off();

    bool took_p_lock = false;
    if (!spinlock_holding(&p->lk)) {
        spinlock_acquire(&p->lk);
        took_p_lock = true;
    }

    // 释放外部锁，避免把它带进 sched()
    spinlock_release(lock);

    p->sleep_space = sleep_space;
    p->state = SLEEPING;

    // 这里要求：持有 p->lk 且中断关闭
    proc_sched();

    // 被唤醒后清理睡眠标记
    p->sleep_space = NULL;

    if (took_p_lock) {
        spinlock_release(&p->lk);
    }

    // 恢复外部锁
    spinlock_acquire(lock);

    pop_off();  // 恢复最外层中断状态
}



/*
    唤醒所有等待 sleep_space 的进程。
    关键点：如果当前 CPU 已经持有某个进程的 p->lk，就不要二次 acquire，
    直接在“已持有”的前提下检查并修改状态；否则再去 acquire/release。
*/
void proc_wakeup(void *sleep_space)
{
    for (int i = 0; i < N_PROC; i++) {
        proc_t *p = &proc_list[i];

        if (spinlock_holding(&p->lk)) {
            // 当前 CPU 已经持有这把锁，避免二次加锁
            if (p->state == SLEEPING && p->sleep_space == sleep_space) {
                p->state = RUNNABLE;
            }
        } else {
            spinlock_acquire(&p->lk);
            if (p->state == SLEEPING && p->sleep_space == sleep_space) {
                p->state = RUNNABLE;
            }
            spinlock_release(&p->lk);
        }
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
    cpu_t *c = mycpu();
    proc_t *p = c->proc;

    // 必须：进入 sched 时中断关闭，且持有 p->lk，且 p 不是 RUNNING
    assert(p != NULL, "proc_sched: no process running");
    assert(spinlock_holding(&p->lk), "proc_sched: not holding lock");
    assert(p->state != RUNNING, "proc_sched: process still running");
    assert(intr_get() == 0, "proc_sched: interrupts must be off");

    // 调度器上下文要合法
    assert(c->ctx.sp != 0 && c->ctx.ra != 0, "proc_sched: invalid scheduler ctx");

    // 切换到调度器；注意：sched 返回到调用者时，仍应保持“中断关闭+依然持有 p->lk”的语义，
    // 由调用者在合适位置释放 p->lk，并再决定是否开中断。
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

    while (1) {
        intr_off();  // 调度循环内禁止中断，避免被时钟打断造成时序竞态

        for (int i = 0; i < N_PROC; i++) {
            p = &proc_list[i];

            // —— 关键改动：条件获取，避免“同核已持有时再 acquire” ——
            bool got_here = false;
            if (!spinlock_holding(&p->lk)) {
                spinlock_acquire(&p->lk);
                got_here = true;
            }

            if (p->state == RUNNABLE) {
                // 将其置为 RUNNING，绑定到当前 CPU
                p->state = RUNNING;
                c->proc = p;

                printf("proc %d is running...\n", p->pid);

                // 切到进程（注意：返回到这里时，按照约定 p->lk 仍然是“被持有”的，
                // 但可能是由不同路径持有；因此释放动作只在 got_here==true 时做）
                swtch(&c->ctx, &p->ctx);
            }

            // 只有本轮是我们获取到的锁，才由我们来释放，防止重复释放/跨路径释放
            if (got_here) {
                spinlock_release(&p->lk);
            }
        }

        intr_on();
    }
}
