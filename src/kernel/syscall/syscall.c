#include "mod.h"

// 跳转表: 系统调用号 -> 系统调用服务函数
static uint64 (*syscalls[])(void) = {
    [SYS_brk]          = sys_brk,
    [SYS_mmap]         = sys_mmap,
    [SYS_munmap]       = sys_munmap,
    [SYS_print_str]    = sys_print_str,
    [SYS_print_int]    = sys_print_int,
    [SYS_getpid]       = sys_getpid,
    [SYS_fork]         = sys_fork,
    [SYS_wait]         = sys_wait,
    [SYS_exit]         = sys_exit,
    [SYS_sleep]        = sys_sleep,

    // **新增：磁盘/缓冲/bitmap 相关 syscall**
    [SYS_alloc_block]  = sys_alloc_block,
    [SYS_free_block]   = sys_free_block,
    [SYS_alloc_inode]  = sys_alloc_inode,
    [SYS_free_inode]   = sys_free_inode,
    [SYS_show_bitmap]  = sys_show_bitmap,
    [SYS_get_block]    = sys_get_block,
    [SYS_read_block]   = sys_read_block,
    [SYS_write_block]  = sys_write_block,
    [SYS_put_block]    = sys_put_block,
    [SYS_show_buffer]  = sys_show_buffer,
    [SYS_flush_buffer] = sys_flush_buffer,
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
