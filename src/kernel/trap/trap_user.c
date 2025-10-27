#include "mod.h"
#include "../lib/method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../proc/method.h"
#include "../proc/type.h"
#include "../arch/method.h"
#include "../../user/syscall_num.h"

// from trampoline.S
extern char trampoline[];
extern char user_vector[];
extern void user_return(trapframe_t *tf, uint64 satp);

// S-mode interrupt helpers (already defined elsewhere)
extern void timer_interrupt_handler(void);
extern void external_interrupt_handler(void);

// kernel trap entry (in trap.S)
extern void kernel_vector(void);

// --------------------------------------------------
// tiny hex printer for low-level UART debug
static void printhex64(uint64 x)
{
    for (int i = 60; i >= 0; i -= 4) {
        int d = (x >> i) & 0xF;
        if (d < 10)
            uart_putc_sync('0' + d);
        else
            uart_putc_sync('a' + d - 10);
    }
}

// dump current trap-related CSRs
static void dump_trap_state(void)
{
    uint64 satp_val   = r_satp();
    uint64 sepc_val   = r_sepc();
    uint64 scause_val = r_scause();
    uint64 stval_val  = r_stval();

    // prefix "S" so we can see trap entries in the log stream
    uart_putc_sync('S');

    uart_putc_sync('[');
    uart_putc_sync('d'); uart_putc_sync('b'); uart_putc_sync('g');
    uart_putc_sync(']');
    uart_putc_sync(' ');

    // satp=
    uart_putc_sync('s'); uart_putc_sync('a'); uart_putc_sync('t'); uart_putc_sync('p'); uart_putc_sync('=');
    printhex64(satp_val);
    uart_putc_sync(' ');

    // pc=
    uart_putc_sync('p'); uart_putc_sync('c'); uart_putc_sync('=');
    printhex64(sepc_val);
    uart_putc_sync(' ');

    // cause=
    uart_putc_sync('c'); uart_putc_sync('a'); uart_putc_sync('u'); uart_putc_sync('s'); uart_putc_sync('e'); uart_putc_sync('=');
    printhex64(scause_val);
    uart_putc_sync(' ');

    // val=
    uart_putc_sync('v'); uart_putc_sync('a'); uart_putc_sync('l'); uart_putc_sync('=');
    printhex64(stval_val);
    uart_putc_sync('\n');
}

// --------------------------------------------------
// return_to_user:
//  called both on first entry (trap_user_return) and after a trap (trap_user_handler)
//  sets up CSRs so that trampoline:user_return will restore user regs and sret to U.
//
//  contract:
//    - p->tf is the trapframe for this proc
//    - tf->sp etc. already prepared
//    - sepc has already been set to the user PC we want to resume (caller responsibility)
static void return_to_user(proc_t *p)
{
    trapframe_t *tf = p->tf;

    // sscratch = trapframe (给下次 user_vector 用)
    w_sscratch((uint64)tf);

    // stvec = user_vector (用户态trap先进trampoline)
    w_stvec((uint64)user_vector);

    // 允许时钟类中断，先别让外部中断(SEIE)打断用户
    uint64 sie = r_sie();
    sie |= (SIE_STIE | SIE_SSIE);   // 计时器/软件中断
    sie &= ~SIE_SEIE;               // 先禁外部中断 (PLIC/UART)
    w_sie(sie);

    // 设定 sstatus：
    //  - SPP=0: sret 之后去 U-mode
    //  - SPIE=1: 允许下次再陷回 S-mode 时 SIE 会自动开
    //  - SIE=0: 现在先别开全局中断，避免我们还在过渡期就被重入
    uint64 sstatus = r_sstatus();
    sstatus &= ~SSTATUS_SPP;   // 目标是 U
    sstatus |=  SSTATUS_SPIE;  // 以后允许 S 中断
    sstatus &= ~SSTATUS_SIE;   // 关键：此刻保持关中断，避免重入风暴
    w_sstatus(sstatus);

    // 调试标记：准备真正回到用户态
    uart_putc_sync('U');

    // 跳到 trampoline.user_return：
    //   - 它会切用户页表 satp
    //   - 恢复用户寄存器
    //   - sret
    user_return(tf, MAKE_SATP(p->pgtbl));

    __builtin_unreachable();
}


// --------------------------------------------------
// trap_user_handler:
//  called AFTER user_vector in trampoline.S has:
//    - saved user regs into tf
//    - switched to kernel page table
//    - jumped here with a0 still == tf (and myproc()->tf == tf)
//
//  here we:
//    - classify trap (interrupt vs syscall)
//    - handle it
//    - set up to return_to_user()
void trap_user_handler(void)
{
    // FIRST THING: make sure further traps (e.g. timer ticks while we're in S)
    // go through the full kernel path, not trampoline again.
    // i.e. in-kernel we want stvec = kernel_vector.
    w_stvec((uint64)kernel_vector);

    proc_t *p = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_handler: null trapframe");

    dump_trap_state(); // low-level state dump each time we enter

    uint64 scause  = r_scause();
    uint64 sepc    = r_sepc();
    uint64 is_intr = (scause >> 63) & 1;
    uint64 code    = scause & 0xfff;

    if (is_intr) {
        // -------------------------
        // Asynchronous interrupt
        // -------------------------
        if (code == 5 || code == 1) {
            // S-mode timer interrupt (STIP=5) OR software interrupt (SSIP=1)
            // Some designs forward M-timer via SSIP=1, others use STIP=5.
            uart_putc_sync('T'); // prove timer fired
            timer_interrupt_handler();

            // resume the same user PC
            w_sepc(sepc);

        } else if (code == 9) {
            // S-mode external interrupt (PLIC -> UART, etc.)
            external_interrupt_handler();

            // resume same user PC
            w_sepc(sepc);

        } else {
            printf("[usertrap] unexpected S interrupt code=%d\n", (int)code);
            // still try to continue at same PC
            w_sepc(sepc);
        }

    } else {
        // -------------------------
        // Synchronous exception
        // -------------------------
        if (code == 8) {
            // U-mode ecall (syscall)
            uart_putc_sync('E'); // syscall marker

            uint64 num = tf->a7;  // by convention: syscall number in a7
            if (num == SYS_helloworld) {
                printf("proczero: hello world!\n");
            } else {
                printf("[user] unknown syscall %lu\n", num);
            }

            // skip ecall so we don't immediately trap again
            w_sepc(sepc + 4);

        } else {
            // unhandled exception from user
            printf("[usertrap] unhandled sync scause=%p sepc=%p stval=%p\n",
                   (void*)scause, (void*)sepc, (void*)r_stval());
            panic("unhandled user exception");
        }
    }

    // All handling done; go resume user mode.
    return_to_user(p);
}

// --------------------------------------------------
// trap_user_return:
//  called once when we FIRST drop into user mode (proc_make_first -> swtch -> here).
//  We set initial sepc to the process entry point, then share the same
//  return_to_user() path to actually jump into user.
void trap_user_return(void)
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_return: null trapframe");

    // user entry point was stashed by proc_make_first() in user_to_kern_epc
    uint64 entry = tf->user_to_kern_epc;
    w_sepc(entry);

    // now just do the same finalization we do after a trap
    return_to_user(p);
}
