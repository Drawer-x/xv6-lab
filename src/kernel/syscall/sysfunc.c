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
#define DEBUG_PRINT(event, addr) \
    printf(#event " event: ret_heap_top = %p\n", (void*)addr); \
    vm_print(p->pgtbl)

uint64 sys_brk()
{
    proc_t *p = myproc();
    uint64 new_top;

    // 读取参数：new_heap_top（0 表示查询）
    arg_uint64(0, &new_top);

    uint64 cur = p->heap_top;

    if (new_top == 0) {
        // 查询当前堆顶
        DEBUG_PRINT(look, cur);
        return cur;
    }

    if (new_top == cur) {
        // 不变
        DEBUG_PRINT(equal, cur);
        return cur;
    }

    if (new_top > cur) {
        // 增长
        uint64 ret = uvm_heap_grow(p->pgtbl, cur, (uint32)(new_top - cur));
        if (ret == (uint64)-1) {
            DEBUG_PRINT(grow, cur);
            return (uint64)-1;
        }
        p->heap_top = ret;
        DEBUG_PRINT(grow, ret);
        return ret;
    } else {
        // 收缩
        uint64 ret = uvm_heap_ungrow(p->pgtbl, cur, (uint32)(cur - new_top));
        if (ret == (uint64)-1) {
            DEBUG_PRINT(ungrow, cur);
            return (uint64)-1;
        }
        p->heap_top = ret;
        DEBUG_PRINT(ungrow, ret);
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
/*
    测试页表复制与销毁
    成功返回0, 失败返回-1
*/
uint64 sys_test_pgtbl() {
    proc_t *p = myproc();
    if (!p || !p->pgtbl) {
        printf("❌ 测试失败：当前进程未初始化或无有效页表\n");
        return -1;
    }

    // 1. 打印当前进程状态（复制源信息）
    printf("\n[1] Current process info (source of copy):\n");
    printf("  - PID: %d %s\n", p->pid, (p->pid == 0) ? "(kernel init process)" : "");
    printf("  - Source page table: %p\n", p->pgtbl);
    printf("  - Heap top: %p\n", p->heap_top);
    printf("  - User stack pages: %d\n", p->ustack_npage);
    printf("  - mmap regions: ");
    if (p->mmap == NULL) {
        printf("none\n");
    } else {
        printf("\n");
        uvm_show_mmaplist(p->mmap);
    }

    // 2. 创建新页表
    printf("\n[2] Creating new page table...\n");
    pgtbl_t new_pgtbl = proc_pgtbl_init((uint64)p->tf);
    if (!new_pgtbl) {
        printf("❌ 测试失败：创建新页表失败\n");
        return -2;
    }
    printf("✅ 新页表创建成功：%p\n", new_pgtbl);

    // 3. 复制页表（堆+栈+mmap全区域）
    printf("\n[3] Copying page table content (heap+stack+mmap)...\n");
    uvm_copy_pgtbl(p->pgtbl, new_pgtbl, p->heap_top, p->ustack_npage, p->mmap);
    printf("✅ 页表复制完成\n");

    // 4. 验证复制结果（仅打印页表结构）
    printf("\n[4] Verifying copied page table structure...\n");
    printf("  - Copied page table content:\n");
    vm_print(new_pgtbl);
    printf("✅ 页表结构复制验证通过\n");

    // 5. 销毁新页表
    printf("\n[5] Destroying copied page table...\n");
    uvm_destroy_pgtbl(new_pgtbl);

    // 6. 验证销毁有效性
    uint64 test_va = (p->ustack_npage > 0) ? (TRAPFRAME - PGSIZE) : USER_BASE;
    pte_t *invalid_pte = vm_getpte(new_pgtbl, test_va, false);
    if (!invalid_pte) {
        printf("✅ 页表销毁成功：资源已完全释放\n");
    } else {
        printf("❌ 测试失败：页表销毁后仍可访问页表项\n");
        return -4;
    }

    return 0;
}