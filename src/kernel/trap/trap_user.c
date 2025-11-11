#include "mod.h"
#include "../arch/mod.h"
#include "../mem/mod.h"

// in trampoline.S
extern char trampoline[];  // 内核和用户切换的代码
extern char user_vector[]; // 用户触发陷阱进入内核（trampoline内偏移）
extern char user_return[]; // 内核处理完毕返回用户（trampoline内偏移）

// in trap.S
extern char kernel_vector[]; // 内核态trap处理流程, 进入内核后应当切换中断处理入口


// in trap_kernel.c
extern char *interrupt_info[16]; // 中断错误信息
extern char *exception_info[16]; // 异常错误信息


// 在user_vector()里面调用
// 用户态trap处理的核心逻辑
void trap_user_handler()
{
	// 进入内核，切换trap入口到kernel_vector
	w_stvec((uint64)kernel_vector);

	proc_t *p = myproc();
	trapframe_t *tf = p->tf;

	// 记录本次trap发生时的用户PC
	tf->user_to_kern_epc = r_sepc();

	uint64 scause = r_scause();
	int trap_id = scause & 0xf;

	if (scause & 0x8000000000000000ul) {
		// 来自U态时的中断：委托给内核的中断处理逻辑
		switch (trap_id) {
		case 1: // S-mode software interrupt（由M态时钟中断转发）
			timer_interrupt_handler();
			break;
		case 9: // S-mode external interrupt（PLIC）
			external_interrupt_handler();
			break;
		default:
			printf("unexpected user interrupt id=%d\n", trap_id);
			printf("sepc=%p stval=%p\n", tf->user_to_kern_epc, r_stval());
			// 未识别的中断：先不panic，直接返回用户态以避免系统退出
			break;
		}
	} else {
	    switch (trap_id) {
	    case 8: // ecall from U-mode
	        // 保存用户程序的返回地址（ecall指令的下一条指令）
	        tf->user_to_kern_epc += 4; // 跳过 ecall
	        
	        // 调用系统调用处理函数
	        syscall();
	        break;
	    case 13: // Load Page Fault
	    case 15: // Store/AMO Page Fault
	    {
	        uint64 stval = r_stval();
	        printf("page fault occured! trap id = %d\n", trap_id);
	        uint64 old_n = p->ustack_npage;
	        uint64 new_n = uvm_ustack_grow(p->pgtbl, p->ustack_npage, stval);
	        if (new_n == (uint64)-1) {
	            printf("ustack grow failed: npage=%p, stval=%p\n", p->ustack_npage, stval);
	            panic("trap_user_handler");
	        }
	        p->ustack_npage = new_n;
	        printf("ustack_npage:  %d -> %d\n", (int)old_n, (int)new_n);
	        break;
	    }
	    default:
	        printf("unexpected user exception id=%d sepc=%p stval=%p\n", trap_id, tf->user_to_kern_epc, r_stval());
	        panic("trap_user_handler");
	    }
	}

	trap_user_return();
}

// 调用user_return()
// 内核态返回用户态
void trap_user_return()
{
	proc_t *p = myproc();
	trapframe_t *tf = p->tf;

	// stvec -> 用户向量（高地址）
	uint64 uservec_va = TRAMPOLINE + ((uint64)user_vector - (uint64)trampoline);
	w_stvec(uservec_va);

	// sscratch 写入 TRAPFRAME 虚拟地址
	w_sscratch((uint64)TRAPFRAME);

	// sret 到 U：清 SPP，置 SPIE
	uint64 s = r_sstatus();
	s &= ~SSTATUS_SPP;
	s |= SSTATUS_SPIE;
	w_sstatus(s);

	// 设置 sepc
	w_sepc(tf->user_to_kern_epc);

	// 跳转 trampoline 的 user_return(trapframe_va, user_satp)
	uint64 userret_va = TRAMPOLINE + ((uint64)user_return - (uint64)trampoline);
	void (*ureturn)(trapframe_t *, uint64) = (void (*)(trapframe_t *, uint64))userret_va;
	ureturn((trapframe_t *)TRAPFRAME, MAKE_SATP(p->pgtbl));
}