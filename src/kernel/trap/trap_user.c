#include "mod.h"
#include "../lib/method.h"
#include "../mem/method.h"
#include "../proc/method.h"
#include "../proc/type.h"
#include "../../user/syscall_num.h"

// 汇编符号
extern char trampoline[];
extern char user_vector[];
extern void user_return(trapframe_t *tf, uint64 satp);

// CSR helpers（仅限本文件）
static inline uint64 r_scause(){ uint64 x; asm volatile("csrr %0, scause" : "=r"(x)); return x; }
static inline uint64 r_sepc()  { uint64 x; asm volatile("csrr %0, sepc"   : "=r"(x)); return x; }
static inline uint64 r_stval() { uint64 x; asm volatile("csrr %0, stval"  : "=r"(x)); return x; }
static inline void   w_sepc(uint64 x){ asm volatile("csrw sepc, %0" : : "r"(x)); }
static inline void   w_stvec(uint64 x){ asm volatile("csrw stvec, %0" : : "r"(x)); }
static inline void   w_sscratch(uint64 x){ asm volatile("csrw sscratch, %0" : : "r"(x)); }
static inline uint64 MAKE_SATP_LOCAL(pgtbl_t p){ return MAKE_SATP(p); } // 复用你在 mem/type.h 的宏

/*
 * 用户态陷阱入口（由 trampoline.S 的 user_vector 跳转进入）
 * 注意：user_vector 是“jr t0”跳到这里的，并不是调用指令，所以**不能 return**到 caller。
 * 处理完成后必须调用 user_return(tf, MAKE_SATP(proc->pgtbl)) 回到用户态。
 */
void trap_user_handler()
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_handler: null trapframe");

    uint64 scause = r_scause();
    uint64 is_intr = (scause >> 63) & 1;
    uint64 code   = scause & 0xfff;

    if (!is_intr) {
        // ---- 异常 ----
        if (code == 8) { // ecall from U
            uint64 num = tf->a7; // 按你的trapframe定义，a7在偏移168
            switch (num) {
                case SYS_helloworld:
                    printf("proczero: hello world\n");
                    break;
                default:
                    printf("user syscall unknown: %lu\n", num);
                    break;
            }
            // 设置“返回用户态”的 sepc：跳过 ecall 指令
            tf->user_to_kern_epc = r_sepc() + 4;
        } else {
            // 其他异常：当前实验直接报错（实际系统里应杀进程/上报）
            printf("user exception: scause=%lx sepc=%lx stval=%lx\n", scause, r_sepc(), r_stval());
            panic("unhandled user exception");
        }
    } else {
        // ---- 中断 ----（本实验不处理，直接返回用户）
        // 需要的话可以在这里调度 timer/external/software 的处理逻辑
        tf->user_to_kern_epc = r_sepc(); // 不改PC，直接回去
    }

    // 关键：跳回用户态（必须调用，不可return）
    //  - stvec 设置为 user_vector 的 VA（在 trampoline 同一页）
    //  - sscratch 需要在返回前持有 tf 指针（trampoline 在 user_vector 首条指令会交换它）
    w_stvec((uint64)user_vector);
    w_sscratch((uint64)tf);

    user_return(tf, MAKE_SATP_LOCAL(p->pgtbl));
    __builtin_unreachable();
}

/*
 * 首次从内核“进入用户态”的出口（由 proczero 的 ctx.ra 调用）
 * 作用与 xv6 的 usertrapret 类似：设置 stvec/sscratch，然后跳到 user_return。
 */
void trap_user_return()
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_return: null trapframe");

    // 首次进入用户态：
    // - sepc 用 tf->user_to_kern_epc（在 proc_make_first 中已设为用户入口）
    // - stvec 指向 user_vector（用户态发生trap时能进 trampoline）
    // - sscratch = tf（trampoline 依赖它取回 tf 指针）
    w_sepc(tf->user_to_kern_epc);
    w_stvec((uint64)user_vector);
    w_sscratch((uint64)tf);

    user_return(tf, MAKE_SATP_LOCAL(p->pgtbl));
    __builtin_unreachable();
}
