#include "mod.h"
#include "../proc/mod.h"
#include "../trap/mod.h"
#include "../mem/mod.h"
#include "../fs/mod.h"

/*
    调整堆边界
    uint64 new_top: 0 表示查询
    返回: 当前(调整后的)堆顶
*/
uint64 sys_brk()
{
    proc_t *p = myproc();
    uint64 new_top;

    arg_uint64(0, &new_top);

    uint64 cur = p->heap_top;

    if (new_top == 0) {
        return cur;
    }

    if (new_top == cur) {
        return cur;
    }

    if (new_top > cur) {
        uint64 ret = uvm_heap_grow(p->pgtbl, cur, (uint32)(new_top - cur), PTE_R | PTE_W);
        if (ret == (uint64)-1) {
            return (uint64)-1;
        }
        p->heap_top = ret;
        return ret;
    } else {
        uint64 ret = uvm_heap_ungrow(p->pgtbl, cur, (uint32)(cur - new_top));
        if (ret == (uint64)-1) {
            return (uint64)-1;
        }
        p->heap_top = ret;
        return ret;
    }
}

/*
    申请一段离散内存映射
    uint64 begin 起始地址(0表示自动找)
    uint32 len   范围 (字节, 必须是page-aligned)
    成功返回0 失败返回-1
*/
uint64 sys_mmap()
{
    uint64 begin;
    uint32 len;

    arg_uint64(0, &begin);
    arg_uint32(1, &len);

    assert((len % PGSIZE) == 0, "sys_mmap: len not aligned");

    proc_t *p = myproc();
    uint64 npages = len / PGSIZE;

    if (begin == 0) {
        begin = p->heap_top;
        p->heap_top += len;
    }

    uvm_mmap(begin, npages, PTE_R | PTE_W);

    return begin;
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节)
    成功返回0 失败返回-1
*/
uint64 sys_munmap()
{
    uint64 begin;
    uint32 len;

    arg_uint64(0, &begin);
    arg_uint32(1, &len);

    assert((len % PGSIZE) == 0, "sys_munmap: len not aligned");

    uvm_munmap(begin, len / PGSIZE);

    return 0;
}

/*
    进程复制（fork）
    返回：子进程PID（父进程），0（子进程），失败返回-1
*/
uint64 sys_fork()
{
    int ret = proc_fork();
    if (ret < 0) return (uint64)-1;
    
    // 子进程返回0
    proc_t *p = myproc();
    if (p->tf->a0 == 0) {
        // 这是子进程，fork后a0已经被设置
    }
    
    return (uint64)ret;
}

/*
    等待子进程退出
    uint64 addr_exit_state: 用户态地址，用于存储子进程退出码
    返回：退出的子进程PID，失败返回-1
*/
uint64 sys_wait()
{
    uint64 addr_exit_state;
    arg_uint64(0, &addr_exit_state);

    int child_pid = proc_wait(addr_exit_state);
    if (child_pid < 0) {
        return (uint64)-1;
    }

    return (uint64)child_pid;
}

/*
    进程退出
    int exit_code: 退出码
    不返回
*/
uint64 sys_exit()
{
    int exit_code;
    arg_uint32(0, (uint32 *)&exit_code);

    proc_exit(exit_code);

    return 0; // unreachable
}

/*
    让进程睡眠一段时间
    uint32 ntick (1个tick大约0.1秒)
    成功返回0
*/
uint64 sys_sleep()
{
    uint32 ntick;
    arg_uint32(0, &ntick);
    
    timer_wait(ntick);
    return 0;
}

/*
    返回当前进程的PID
*/
uint64 sys_getpid()
{
    proc_t *p = myproc();
    return (uint64)p->pid;
}

/*
    执行ELF文件
    char *path: ELF文件路径
    char **argv: 参数数组
    成功返回0，失败返回-1
*/
uint64 sys_exec()
{
    char path[STR_MAXLEN];
    uint64 uargv;
    
    arg_str(0, path, STR_MAXLEN);
    arg_uint64(1, &uargv);
    
    // 读取用户态的argv数组
    char *argv[32];
    int argc = 0;
    
    if (uargv != 0) {
        proc_t *p = myproc();
        for (argc = 0; argc < 31; argc++) {
            uint64 uarg;
            uvm_copyin(p->pgtbl, (uint64)&uarg, uargv + argc * sizeof(uint64), sizeof(uint64));
            if (uarg == 0) break;
            
            argv[argc] = (char*)pmem_alloc(true);
            uvm_copyin_str(p->pgtbl, (uint64)argv[argc], uarg, PGSIZE);
        }
    }
    argv[argc] = NULL;
    
    int ret = proc_exec(path, argv);
    
    // 释放临时分配的内存
    for (int i = 0; i < argc; i++) {
        pmem_free((uint64)argv[i], true);
    }
    
    return (uint64)ret;
}

/*
    打开文件
    char *path: 文件路径
    uint32 mode: 打开模式
    返回：文件描述符fd，失败返回-1
*/
uint64 sys_open()
{
    char path[STR_MAXLEN];
    uint32 mode;
    
    arg_str(0, path, STR_MAXLEN);
    arg_uint32(1, &mode);
    
    file_t *f = file_open(path, mode);
    if (f == NULL) {
        return (uint64)-1;
    }
    
    // 在进程的打开文件表中找一个空位
    proc_t *p = myproc();
    for (int fd = 0; fd < 16; fd++) {
        if (p->open_file[fd] == NULL) {
            p->open_file[fd] = f;
            return (uint64)fd;
        }
    }
    
    // 没有空位，关闭文件
    file_close(f);
    return (uint64)-1;
}

/*
    关闭文件
    int fd: 文件描述符
    返回：成功0，失败-1
*/
uint64 sys_close()
{
    uint32 fd;
    arg_uint32(0, &fd);
    
    if (fd >= 16) return (uint64)-1;
    
    proc_t *p = myproc();
    file_t *f = p->open_file[fd];
    if (f == NULL) return (uint64)-1;
    
    file_close(f);
    p->open_file[fd] = NULL;
    
    return 0;
}

/*
    读取文件
    int fd: 文件描述符
    uint32 len: 读取长度
    uint64 addr: 缓冲区地址
    返回：实际读取的字节数
*/
uint64 sys_read()
{
    uint32 fd;
    uint32 len;
    uint64 addr;
    
    arg_uint32(0, &fd);
    arg_uint32(1, &len);
    arg_uint64(2, &addr);
    
    if (fd >= 16) return (uint64)-1;
    
    proc_t *p = myproc();
    file_t *f = p->open_file[fd];
    if (f == NULL) return (uint64)-1;
    
    return (uint64)file_read(f, len, addr, true);
}

/*
    写入文件
    int fd: 文件描述符
    uint32 len: 写入长度
    uint64 addr: 缓冲区地址
    返回：实际写入的字节数
*/
uint64 sys_write()
{
    uint32 fd;
    uint32 len;
    uint64 addr;
    
    arg_uint32(0, &fd);
    arg_uint32(1, &len);
    arg_uint64(2, &addr);
    
    proc_t *p = myproc();
    
    if (fd >= 16) {
        return (uint64)-1;
    }
    
    file_t *f = p->open_file[fd];
    if (f == NULL) {
        return (uint64)-1;
    }
    
    return (uint64)file_write(f, len, addr, true);
}

/*
    移动读写指针
    int fd: 文件描述符
    uint32 offset: 偏移量
    uint32 whence: 基准位置
    返回：新的偏移量
*/
uint64 sys_lseek()
{
    uint32 fd;
    uint32 offset;
    uint32 whence;
    
    arg_uint32(0, &fd);
    arg_uint32(1, &offset);
    arg_uint32(2, &whence);
    
    if (fd >= 16) return (uint64)-1;
    
    proc_t *p = myproc();
    file_t *f = p->open_file[fd];
    if (f == NULL) return (uint64)-1;
    
    return (uint64)file_lseek(f, offset, whence);
}

/*
    复制文件描述符
    int fd: 原文件描述符
    返回：新的文件描述符
*/
uint64 sys_dup()
{
    uint32 fd;
    arg_uint32(0, &fd);
    
    if (fd >= 16) return (uint64)-1;
    
    proc_t *p = myproc();
    file_t *f = p->open_file[fd];
    if (f == NULL) return (uint64)-1;
    
    // 找一个空位
    for (int newfd = 0; newfd < 16; newfd++) {
        if (p->open_file[newfd] == NULL) {
            p->open_file[newfd] = file_dup(f);
            return (uint64)newfd;
        }
    }
    
    return (uint64)-1;
}

/*
    获取文件状态
    int fd: 文件描述符
    struct stat *st: 状态结构体
    返回：成功0，失败-1
*/
uint64 sys_fstat()
{
    uint32 fd;
    uint64 st;
    
    arg_uint32(0, &fd);
    arg_uint64(1, &st);
    
    if (fd >= 16) return (uint64)-1;
    
    proc_t *p = myproc();
    file_t *f = p->open_file[fd];
    if (f == NULL) return (uint64)-1;
    
    return (uint64)file_get_stat(f, st);
}

/*
    获取目录下所有有效目录项
    int fd: 目录的文件描述符
    char *buf: 缓冲区
    uint32 len: 缓冲区大小
    返回：读取的字节数
*/
uint64 sys_get_dentries()
{
    uint32 fd;
    uint64 buf;
    uint32 len;
    
    arg_uint32(0, &fd);
    arg_uint64(1, &buf);
    arg_uint32(2, &len);
    
    if (fd >= 16) return (uint64)-1;
    
    proc_t *p = myproc();
    file_t *f = p->open_file[fd];
    if (f == NULL || f->ip == NULL) return (uint64)-1;
    
    inode_t *ip = f->ip;
    inode_lock(ip);
    
    if (ip->disk_info.type != INODE_TYPE_DIR) {
        inode_unlock(ip);
        return (uint64)-1;
    }
    
    uint32 ret = dentry_transmit(ip, buf, len, true);
    inode_unlock(ip);
    
    return (uint64)ret;
}

/*
    创建目录
    char *path: 目录路径
    返回：成功0，失败-1
*/
uint64 sys_mkdir()
{
    char path[STR_MAXLEN];
    arg_str(0, path, STR_MAXLEN);
    
    inode_t *ip = path_create_inode(path, INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
    if (ip == NULL) {
        return (uint64)-1;
    }
    
    inode_put(ip);
    return 0;
}

/*
    切换工作目录
    char *path: 目录路径
    返回：成功0，失败-1
*/
uint64 sys_chdir()
{
    char path[STR_MAXLEN];
    arg_str(0, path, STR_MAXLEN);
    
    inode_t *ip = path_to_inode(path);
    if (ip == NULL) {
        return (uint64)-1;
    }
    
    inode_lock(ip);
    if (ip->disk_info.type != INODE_TYPE_DIR) {
        inode_unlock(ip);
        inode_put(ip);
        return (uint64)-1;
    }
    inode_unlock(ip);
    
    proc_t *p = myproc();
    if (p->cwd != NULL) {
        inode_put(p->cwd);
    }
    p->cwd = ip;
    
    return 0;
}

/*
    打印工作目录的绝对路径
    返回：路径长度
*/
uint64 sys_print_cwd()
{
    proc_t *p = myproc();
    if (p->cwd == NULL) {
        printf("/\n");
        return 1;
    }
    
    char path[256];
    uint32 offset = inode_to_path(p->cwd, path, 256);
    printf("%s\n", path + offset);
    
    return 256 - offset;
}

/*
    建立硬链接
    char *old_path: 原路径
    char *new_path: 新路径
    返回：成功0，失败-1
*/
uint64 sys_link()
{
    char old_path[STR_MAXLEN];
    char new_path[STR_MAXLEN];
    
    arg_str(0, old_path, STR_MAXLEN);
    arg_str(1, new_path, STR_MAXLEN);
    
    return (uint64)path_link(old_path, new_path);
}

/*
    解除硬链接
    char *path: 路径
    返回：成功0，失败-1
*/
uint64 sys_unlink()
{
    char path[STR_MAXLEN];
    arg_str(0, path, STR_MAXLEN);
    
    return (uint64)path_unlink(path);
}

/*
    获取内存状态
    uint64 kern_free_addr: 内核空闲页数的用户态地址
    uint64 user_free_addr: 用户空闲页数的用户态地址
    返回：0
*/
uint64 sys_pmem_stat()
{
    uint64 kern_addr, user_addr;
    arg_uint64(0, &kern_addr);
    arg_uint64(1, &user_addr);
    
    uint32 kern_free, user_free;
    pmem_stat(&kern_free, &user_free);
    
    proc_t *p = myproc();
    if (kern_addr != 0) {
        uvm_copyout(p->pgtbl, kern_addr, (uint64)&kern_free, sizeof(uint32));
    }
    if (user_addr != 0) {
        uvm_copyout(p->pgtbl, user_addr, (uint64)&user_free, sizeof(uint32));
    }
    
    return 0;
}
