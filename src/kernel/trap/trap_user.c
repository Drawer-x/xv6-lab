#include "mod.h"
#include "../lib/method.h"
#include "../mem/method.h"
#include "../mem/type.h"
#include "../proc/method.h"
#include "../proc/type.h"
#include "../arch/method.h"      // <-- 加这个，拿 r_satp()/r_sepc()/...
#include "../../user/syscall_num.h"


// from trampoline.S
extern char trampoline[];
extern char user_vector[];
extern void user_return(trapframe_t *tf, uint64 satp);

// 内核里本来就有的中断处理函数
extern void timer_interrupt_handler(void);
extern void external_interrupt_handler(void);

// 小工具：打印 64-bit 十六进制
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

// 打印调试状态：satp / sepc / scause / stval
static void dump_trap_state(void)
{
    uint64 satp_val   = r_satp();
    uint64 sepc_val   = r_sepc();
    uint64 scause_val = r_scause();
    uint64 stval_val  = r_stval();

    uart_putc_sync('[');
    uart_putc_sync('d');
    uart_putc_sync('b');
    uart_putc_sync('g');
    uart_putc_sync(']');
    uart_putc_sync(' ');

    // satp=
    uart_putc_sync('s');
    uart_putc_sync('a');
    uart_putc_sync('t');
    uart_putc_sync('p');
    uart_putc_sync('=');
    printhex64(satp_val);

    uart_putc_sync(' ');

    // sepc=
    uart_putc_sync('p');
    uart_putc_sync('c');
    uart_putc_sync('=');
    printhex64(sepc_val);

    uart_putc_sync(' ');

    // scause=
    uart_putc_sync('c');
    uart_putc_sync('a');
    uart_putc_sync('u');
    uart_putc_sync('s');
    uart_putc_sync('e');
    uart_putc_sync('=');
    printhex64(scause_val);

    uart_putc_sync(' ');

    // stval=
    uart_putc_sync('v');
    uart_putc_sync('a');
    uart_putc_sync('l');
    uart_putc_sync('=');
    printhex64(stval_val);

    uart_putc_sync('\n');
}

// ----------------------------------------------------------------------
// trap_user_handler()
// 说明：这是用户态 trap (ecall / page fault / 中断等) 回到内核后
//       真正的C层入口。最终会再次准备返回用户态。
// ----------------------------------------------------------------------
void trap_user_handler(void)
{
    proc_t *p = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_handler: null trapframe");

    // 探针，表示我们从用户态trap回来了
    *(volatile unsigned char *)UART_BASE = 'S';

    // 新增：立即dump关键信息
    dump_trap_state();

    uint64 scause  = r_scause();
    uint64 sepc    = r_sepc();
    uint64 is_intr = (scause >> 63) & 1;
    uint64 code    = scause & 0xfff;

    if (is_intr) {
        // ========== 异步中断 (S-mode timer / external) ==========
        if (code == 5) {
            // S-mode timer interrupt
            timer_interrupt_handler();
        } else if (code == 9) {
            // S-mode external interrupt (PLIC / UART)
            external_interrupt_handler();
        } else {
            printf("[usertrap] unexpected S interrupt code=%d\n", (int)code);
        }

        // 回用户：从同一条PC继续执行
        asm volatile("csrw sepc, %0" :: "r"(sepc));

    } else {
        // ========== 同步异常 (例如 ecall / fault) ==========
        if (code == 8) {
            // 用户态 ecall
            *(volatile unsigned char *)UART_BASE = 'E';

            uint64 num = tf->a7;  // 约定：a7 = syscall number
            if (num == SYS_helloworld) {
                printf("proczero: hello world!\n");
            } else {
                printf("[user] unknown syscall %lu\n", num);
            }

            // ecall 的 sepc 指向 ecall 指令本身
            // 返回用户时要跳过它，否则会再次陷入
            asm volatile("csrw sepc, %0" :: "r"(sepc + 4));

        } else {
            // 不是我们支持的同步异常：报错+停机
            printf("[usertrap] unhandled sync scause=%p sepc=%p stval=%p\n",
                   (void*)scause, (void*)sepc, (void*)r_stval());
            panic("unhandled user exception");
        }
    }

    // ===== 准备 sret 回用户态 =====

    // 用户态下一次trap时，trampoline 的 user_vector 需要从 sscratch 找到 trapframe
    w_sscratch((uint64)tf);

    // stvec 指向 trampoline 的 user_vector，
    // 这样 U 态再次陷入会跳到 trampoline.S:user_vector
    w_stvec((uint64)user_vector);

    // sstatus:
    //  - SPP=0 : sret 后回到 U 模式
    //  - SPIE=1: sret 后 SIE=1（允许以后打开中断）
    uint64 sstatus2 = r_sstatus();
    sstatus2 &= ~SSTATUS_SPP;
    sstatus2 |=  SSTATUS_SPIE;
    w_sstatus(sstatus2);

    // 我们目前不想在用户态就被时钟/外设中断打断（还没调度/抢占）
    // 所以清掉 SIE 中具体中断位
    uint64 sie2 = r_sie();
    sie2 &= ~(SIE_STIE | SIE_SEIE | SIE_SSIE);
    w_sie(sie2);

    // 真正返回用户态：跳进 trampoline.S:user_return
    // user_return 会恢复通用寄存器，切换到用户页表，并执行 sret
    user_return(tf, MAKE_SATP(p->pgtbl));
    __builtin_unreachable();
}

// ----------------------------------------------------------------------
// trap_user_return()
// 说明：第一次从内核“下放”到用户态时会走这里。
//       它为 sret 准备好 sepc/sstatus/stvec/sscratch/sie，然后跳到 user_return。
// ----------------------------------------------------------------------
void trap_user_return(void)
{
    proc_t *p  = myproc();
    trapframe_t *tf = p->tf;
    assert(tf != NULL, "trap_user_return: null trapframe");

    // 探针：准备首次进入用户态
    *(volatile unsigned char *)UART_BASE = 'R';

    // --- Step 0: 彻底关掉 S-mode 中断全局开关，防止我们调试期间被外部中断打断 ---
    // 清 sstatus.SIE
    uint64 sstatus0 = r_sstatus();
    sstatus0 &= ~SSTATUS_SIE;
    w_sstatus(sstatus0);

    // 只为了 debug，一点点原始输出（不用printf，避免复杂栈/中断窗口）
    uart_putc_sync('['); uart_putc_sync('d'); uart_putc_sync('b'); uart_putc_sync('g'); uart_putc_sync(']');
    uart_putc_sync(' ');
    uart_putc_sync('C'); uart_putc_sync('L'); uart_putc_sync('R'); uart_putc_sync('_'); uart_putc_sync('S'); uart_putc_sync('I'); uart_putc_sync('E');
    uart_putc_sync('\n');

    // 1. sepc <- 用户入口PC
    {
        uint64 entry = tf->user_to_kern_epc;

        uart_putc_sync('R'); // probe 'R' we were already printing
        uart_putc_sync('[');
        uart_putc_sync('d');
        uart_putc_sync('b');
        uart_putc_sync('g');
        uart_putc_sync(']');
        uart_putc_sync(' ');
        uart_putc_sync('s'); uart_putc_sync('e'); uart_putc_sync('t'); uart_putc_sync('t'); uart_putc_sync('i'); uart_putc_sync('n'); uart_putc_sync('g'); uart_putc_sync(' ');
        uart_putc_sync('s'); uart_putc_sync('e'); uart_putc_sync('p'); uart_putc_sync('c'); uart_putc_sync('=');
        printhex64(entry);
        uart_putc_sync('\n');

        asm volatile("csrw sepc, %0" :: "r"(entry));

        uint64 now_sepc = r_sepc();
        uart_putc_sync('['); uart_putc_sync('d'); uart_putc_sync('b'); uart_putc_sync('g'); uart_putc_sync(']');
        uart_putc_sync(' ');
        uart_putc_sync('a'); uart_putc_sync('f'); uart_putc_sync('t'); uart_putc_sync('e'); uart_putc_sync('r'); uart_putc_sync(' ');
        uart_putc_sync('w'); uart_putc_sync('r'); uart_putc_sync('i'); uart_putc_sync('t'); uart_putc_sync('e'); uart_putc_sync(' ');
        uart_putc_sync('r'); uart_putc_sync('_'); uart_putc_sync('s'); uart_putc_sync('e'); uart_putc_sync('p'); uart_putc_sync('c'); uart_putc_sync('('); uart_putc_sync(')'); uart_putc_sync('=');
        printhex64(now_sepc);
        uart_putc_sync('\n');
    }

    // 2. sscratch <- trapframe 指针
    //    这样 user_vector 里 csrrw a0, sscratch, a0 才能拿到 tf
    w_sscratch((uint64)tf);

    // 3. stvec <- user_vector
    //    以后用户态 trap (ecall/中断) 会跳这里
    w_stvec((uint64)user_vector);

    // 4. sstatus:
    //    - 清 SPP，表示 sret 后进入 U 模式
    //    - 置 SPIE，表示 sret 后 SIE=1 (用户态时打开下一轮S中断)
    {
        uint64 sstatus_before = r_sstatus();

        uart_putc_sync('['); uart_putc_sync('d'); uart_putc_sync('b'); uart_putc_sync('g'); uart_putc_sync(']');
        uart_putc_sync(' ');
        uart_putc_sync('s'); uart_putc_sync('s'); uart_putc_sync('t'); uart_putc_sync('a'); uart_putc_sync('t'); uart_putc_sync('u'); uart_putc_sync('s');
        uart_putc_sync('('); uart_putc_sync('b'); uart_putc_sync('e'); uart_putc_sync('f'); uart_putc_sync('o'); uart_putc_sync('r'); uart_putc_sync('e'); uart_putc_sync(')'); uart_putc_sync('=');
        printhex64(sstatus_before);
        uart_putc_sync('\n');

        uint64 sstatus_new = sstatus_before;
        sstatus_new &= ~SSTATUS_SPP;   // sret -> U-mode
        sstatus_new |=  SSTATUS_SPIE;  // set SPIE
        // 注意：我们不在这里重新开 SIE，全局中断仍保持关闭直到 sret 之后
        // (即我们不设置 SIE 位；刚才已经清掉了)
        sstatus_new &= ~SSTATUS_SIE;   // just to be explicit: keep SIE=0 now
        w_sstatus(sstatus_new);

        uint64 sstatus_after = r_sstatus();
        uart_putc_sync('['); uart_putc_sync('d'); uart_putc_sync('b'); uart_putc_sync('g'); uart_putc_sync(']');
        uart_putc_sync(' ');
        uart_putc_sync('s'); uart_putc_sync('s'); uart_putc_sync('t'); uart_putc_sync('a'); uart_putc_sync('t'); uart_putc_sync('u'); uart_putc_sync('s');
        uart_putc_sync('('); uart_putc_sync('a'); uart_putc_sync('f'); uart_putc_sync('t'); uart_putc_sync('e'); uart_putc_sync('r'); uart_putc_sync(')'); uart_putc_sync('=');
        printhex64(sstatus_after);
        uart_putc_sync('\n');
    }

    // 5. 仍然把 sie 的三个子位关掉（双保险）
    {
        uint64 sie = r_sie();
        sie &= ~(SIE_STIE | SIE_SEIE | SIE_SSIE);
        w_sie(sie);
    }

    // 6. 调试：确认 stvec 和 user_vector 是一致的
    {
        uint64 cur_stvec = r_stvec();
        uart_putc_sync('U'); // old probe
        uart_putc_sync('['); uart_putc_sync('d'); uart_putc_sync('b'); uart_putc_sync('g'); uart_putc_sync(']');
        uart_putc_sync(' ');
        printhex64(cur_stvec);
        uart_putc_sync(' ');
        printhex64((uint64)user_vector);
        uart_putc_sync('\n');
    }

    // 7. 进入 trampoline.S:user_return
    //    a0 = tf
    //    a1 = satp 值 (MAKE_SATP(p->pgtbl))
    user_return(tf, MAKE_SATP(p->pgtbl));

    __builtin_unreachable();
}
