// src/kernel/syscall/sysfunc.c
#include "mod.h"

// 一个固定的测试数组，内核要发给用户的
//static int kernel_array[5] = {1, 2, 3, 4, 5};

/*
    测试: 从用户空间传入一个int类型的数组
    uint64 addr 数组起始地址
    uint32 len  元素数量
    成功返回0
*/
uint64 sys_copyin()
{
    proc_t *p = myproc();
    uint64 addr;
    uint32 len;
    
    // 读取参数
    arg_uint64(0, &addr);
    arg_uint32(1, &len);
    
    // 分配内核缓冲区
    int *kernel_buf = (int *)pmem_alloc(true);
    if (kernel_buf == NULL) {
        return -1;
    }
    
    // 从用户空间拷贝数据到内核空间
    uvm_copyin(p->pgtbl, (uint64)kernel_buf, addr, len * sizeof(int));
    
    // 打印数据验证（按照实验要求的格式）
    for (uint32 i = 0; i < len; i++) {
        printf("get a number from user: %d\n", kernel_buf[i]);
    }
    
    // 释放内核缓冲区
    pmem_free((uint64)kernel_buf, true);
    
    return 0;
}

/*
    测试: 向用户空间传出一个int类型的数组
    uint64 addr 数组起始地址
    成功返回拷贝的元素数量
*/
uint64 sys_copyout()
{
    proc_t *p = myproc();
    uint64 addr;
    
    // 读取参数
    arg_uint64(0, &addr);
    
    // 准备要发送的数据
    int kernel_data[5] = {1, 2, 3, 4, 5};
    uint32 len = 5;
    
    // 从内核空间拷贝数据到用户空间
    uvm_copyout(p->pgtbl, addr, (uint64)kernel_data, len * sizeof(int));
    
    return len;
}

/*
    测试: 从用户空间传入一个字符串
    uint64 addr 字符串起始地址
    成功返回0
*/
uint64 sys_copyinstr()
{
    proc_t *p = myproc();
    uint64 addr;
    
    // 读取参数
    arg_uint64(0, &addr);
    
    // 分配内核缓冲区
    char *kernel_buf = (char *)pmem_alloc(true);
    if (kernel_buf == NULL) {
        return -1;
    }
    
    // 从用户空间拷贝字符串到内核空间
    uvm_copyin_str(p->pgtbl, (uint64)kernel_buf, addr, PGSIZE);
    
    // 打印字符串验证（按照实验要求的格式）
    printf("get string for user: %s\n", kernel_buf);
    
    // 释放内核缓冲区
    pmem_free((uint64)kernel_buf, true);
    
    return 0;
}

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

    uint64 old_top = p->heap_top;
    uint64 ret_top = old_top;

    if (new_top == 0)
    {
        // 查询
        printf("[sys_brk] query heap_top=%p\n", (void *)old_top);
        return old_top;
    }

    if (new_top > old_top)
    {
        uint64 grown = uvm_heap_grow(p->pgtbl, old_top, (uint32)(new_top - old_top));
        if (grown == (uint64)-1)
        {
            printf("[sys_brk] grow failed: old=%p want=%p\n",
                   (void *)old_top, (void *)new_top);
            return old_top;
        }
        p->heap_top = grown;
        ret_top     = grown;
        printf("[sys_brk] grow: %p -> %p\n", (void *)old_top, (void *)grown);
    }
    else if (new_top < old_top)
    {
        uint64 ungrown = uvm_heap_ungrow(p->pgtbl, old_top, (uint32)(old_top - new_top));
        p->heap_top = ungrown;
        ret_top     = ungrown;
        printf("[sys_brk] ungrow: %p -> %p\n", (void *)old_top, (void *)ungrown);
    }
    else
    {
        // 不变
        printf("[sys_brk] keep: %p\n", (void *)old_top);
    }

    return ret_top;
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

    uvm_mmap(begin, len / PGSIZE, PTE_R | PTE_W);

    proc_t *p = myproc();
    printf("[sys_mmap] after mmap:\n");
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return 0;
}

/*
    解除一段内存映射
    uint64 start 起始地址
    uint32 len   范围 (字节, 需检查是否是page-aligned)
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

    proc_t *p = myproc();
    printf("[sys_munmap] after munmap:\n");
    uvm_show_mmaplist(p->mmap);
    vm_print(p->pgtbl);
    printf("\n");

    return 0;
}
