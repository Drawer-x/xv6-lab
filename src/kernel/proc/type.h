#pragma once
#include "../arch/type.h"
#include "../lock/type.h" 

// 同优先级的上下文
typedef struct context
{
    uint64 ra; // 返回地址
    uint64 sp; // 栈指针

    // callee-saved通用寄存器
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
} context_t;

// 跨优先级的上下文
typedef struct trapframe {
  // 这 5 个是内核专用，trampoline.S 用固定偏移读写：
  uint64 kernel_satp;    // 0x00  (S-mode 的 satp，返回内核时要切回来)
  uint64 kernel_sp;      // 0x08  (内核栈顶)
  uint64 kernel_trap;    // 0x10  (S-mode trap 入口函数指针：trap_user_handler)
  uint64 epc;            // 0x18  (用户态下一条要执行的 PC)
  uint64 kernel_hartid;  // 0x20  (当前 hartid)

  // 下面按 RISC-V ABI 保存 31 个寄存器，trampoline.S 也用固定偏移
  uint64 ra;
  uint64 sp;
  uint64 gp;
  uint64 tp;
  uint64 t0;
  uint64 t1;
  uint64 t2;
  uint64 s0;
  uint64 s1;
  uint64 a0;
  uint64 a1;
  uint64 a2;
  uint64 a3;
  uint64 a4;
  uint64 a5;
  uint64 a6;
  uint64 a7;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
  uint64 t3;
  uint64 t4;
  uint64 t5;
  uint64 t6;
} trapframe_t;


// 外部结构体
typedef uint64 *pgtbl_t;
typedef struct mmap_region mmap_region_t;


/*
    可能的进程状态转换：
    UNUSED -> RUNNABLE 进程初始化
    RUNNABLE -> RUNNIGN 进程获得CPU使用权
    RUNNING -> RUNNABLE 进程失去CPU使用权
    RUNNING -> SLEEPING 进程睡眠
    SLEEPING -> RUNNABLE 进程苏醒
    RUNNING -> ZOMBIE 进程宣布退出
    ZOMBIE -> UNUSED 进程被父进程回收
*/
enum proc_state
{
    UNUSED,   // 未被使用
    RUNNABLE, // 准备就绪
    RUNNING,  // 运行中
    SLEEPING, // 睡眠等待
    ZOMBIE,   // 濒临死亡
};

// 进程
typedef struct proc
{
    int pid;             // 标识符
    char name[16];       // 进程名称

    spinlock_t lk;         // 自旋锁, 保护下面4个字段
    enum proc_state state; // 进程状态
    struct proc *parent;   // 父进程
    int exit_code;         // 进程退出状态(父进程关心)
    void *sleep_space;     // 进程睡眠位置(等待的资源)

    pgtbl_t pgtbl;       // 用户态页表
    uint64 heap_top;     // 用户堆顶(以字节为单位)
    uint64 ustack_npage; // 用户栈占用的页面数量
    mmap_region_t *mmap; // 用户态mmap区域
    trapframe_t *tf;     // 用户态内核态切换时的运行环境暂存空间

    uint64 kstack;       // 内核栈的虚拟地址
    context_t ctx;       // 内核态进程上下文
} proc_t;

// 系统中最多同时存在N_PROC个进程
#define N_PROC 32
