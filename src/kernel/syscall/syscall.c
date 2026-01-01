#include "mod.h"

// 跳转表: 系统调用号 -> 系统调用服务函数
static uint64 (*syscalls[])(void) = {
    [SYS_brk]          = sys_brk,
    [SYS_mmap]         = sys_mmap,
    [SYS_munmap]       = sys_munmap,
    [SYS_fork]         = sys_fork,
    [SYS_wait]         = sys_wait,
    [SYS_exit]         = sys_exit,
    [SYS_sleep]        = sys_sleep,
    [SYS_getpid]       = sys_getpid,
    [SYS_exec]         = sys_exec,
    [SYS_open]         = sys_open,
    [SYS_close]        = sys_close,
    [SYS_read]         = sys_read,
    [SYS_write]        = sys_write,
    [SYS_lseek]        = sys_lseek,
    [SYS_dup]          = sys_dup,
    [SYS_fstat]        = sys_fstat,
    [SYS_get_dentries] = sys_get_dentries,
    [SYS_mkdir]        = sys_mkdir,
    [SYS_chdir]        = sys_chdir,
    [SYS_print_cwd]    = sys_print_cwd,
    [SYS_link]         = sys_link,
    [SYS_unlink]       = sys_unlink,
    [SYS_pmem_stat]    = sys_pmem_stat,
};

void syscall(void)
{
    proc_t *p = myproc();
    uint64 sysnum = p->tf->a7;
    
    if (sysnum >= 0 && sysnum <= SYS_MAX_NUM && syscalls[sysnum]) {
        uint64 ret = syscalls[sysnum]();
        p->tf->a0 = ret;
    } else {
        printf("unknown syscall %d from pid %d\n", (int)sysnum, p->pid);
        p->tf->a0 = (uint64)-1;
    }
}

/* 参数读取工具函数 */

static uint64 arg_raw(int n)
{
    proc_t *p = myproc();
    switch (n) {
        case 0: return p->tf->a0;
        case 1: return p->tf->a1;
        case 2: return p->tf->a2;
        case 3: return p->tf->a3;
        case 4: return p->tf->a4;
        case 5: return p->tf->a5;
    }
    panic("arg_raw");
    return 0;
}

void arg_uint32(int n, uint32 *ip)
{
    *ip = (uint32)arg_raw(n);
}

void arg_uint64(int n, uint64 *ip)
{
    *ip = arg_raw(n);
}

// 读取 n 号参数指向的字符串到 buf, 字符串最大长度是 maxlen
void arg_str(int n, char *buf, int maxlen)
{
    proc_t *p = myproc();
    uint64 addr;
    arg_uint64(n, &addr);
    uvm_copyin_str(p->pgtbl, (uint64)buf, addr, maxlen);
}
