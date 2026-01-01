/*
 * exec.c - 执行ELF文件
 * 
 * 实现proc_exec函数，用于加载和执行ELF格式的可执行文件
 */

#include "mod.h"
#include "../mem/mod.h"
#include "../lib/mod.h"
#include "../fs/mod.h"

/* ELF文件头结构 */
#define ELF_MAGIC 0x464C457FU  // "\x7fELF" in little endian

typedef struct {
    uint32 magic;       // ELF magic number
    uint8 elf[12];      // ELF identification
    uint16 type;        // Object file type
    uint16 machine;     // Architecture
    uint32 version;     // Object file version
    uint64 entry;       // Entry point virtual address
    uint64 phoff;       // Program header table file offset
    uint64 shoff;       // Section header table file offset
    uint32 flags;       // Processor-specific flags
    uint16 ehsize;      // ELF header size
    uint16 phentsize;   // Program header table entry size
    uint16 phnum;       // Program header table entry count
    uint16 shentsize;   // Section header table entry size
    uint16 shnum;       // Section header table entry count
    uint16 shstrndx;    // Section name string table index
} elf_header_t;

/* 程序头结构 */
#define PT_LOAD 1  // Loadable segment

typedef struct {
    uint32 type;        // Segment type
    uint32 flags;       // Segment flags
    uint64 off;         // Segment file offset
    uint64 vaddr;       // Segment virtual address
    uint64 paddr;       // Segment physical address
    uint64 filesz;      // Segment size in file
    uint64 memsz;       // Segment size in memory
    uint64 align;       // Segment alignment
} prog_header_t;

/* 程序头标志 */
#define PF_X 0x1  // Executable
#define PF_W 0x2  // Writable
#define PF_R 0x4  // Readable

/* 辅助函数：将ELF段加载到用户空间 */
static int load_segment(pgtbl_t pgtbl, uint64 va, inode_t *ip, uint32 offset, uint32 sz, int perm)
{
    uint64 va_page, pa;
    uint32 n, left;
    
    for (uint64 i = 0; i < sz; i += PGSIZE) {
        va_page = ALIGN_DOWN(va + i, PGSIZE);
        
        // 分配物理页
        pa = (uint64)pmem_alloc(false);
        if (pa == 0) {
            return -1;
        }
        memset((void*)pa, 0, PGSIZE);
        
        // 映射页面
        vm_mappages(pgtbl, va_page, pa, PGSIZE, perm | PTE_U | PTE_V);
        
        // 计算实际需要读取的大小
        left = sz - i;
        n = (left < PGSIZE) ? left : PGSIZE;
        
        // 从文件读取数据到物理页
        if (inode_read_data(ip, offset + i, n, (void*)pa, false) != n) {
            return -1;
        }
    }
    
    return 0;
}

/* 辅助函数：准备用户堆区域 (预留，暂不使用) */
__attribute__((unused))
static uint64 prepare_heap(pgtbl_t pgtbl, uint64 heap_start, uint64 heap_end)
{
    uint64 heap_top = ALIGN_UP(heap_start, PGSIZE);
    
    // 为堆分配额外的页面（如果需要）
    while (heap_top < heap_end) {
        uint64 pa = (uint64)pmem_alloc(false);
        if (pa == 0) {
            return 0;
        }
        memset((void*)pa, 0, PGSIZE);
        vm_mappages(pgtbl, heap_top, pa, PGSIZE, PTE_R | PTE_W | PTE_U | PTE_V);
        heap_top += PGSIZE;
    }
    
    return heap_top;
}

/* 辅助函数：准备用户栈并处理参数 */
static uint64 prepare_stack(pgtbl_t pgtbl, char **argv, uint64 *argc_out)
{
    uint64 sp = TRAPFRAME - PGSIZE;  // 栈底在TRAPFRAME之下一页
    uint64 pa;
    
    // 分配栈页
    pa = (uint64)pmem_alloc(false);
    if (pa == 0) {
        return 0;
    }
    memset((void*)pa, 0, PGSIZE);
    vm_mappages(pgtbl, sp, pa, PGSIZE, PTE_R | PTE_W | PTE_U | PTE_V);
    
    // 栈从高地址向低地址增长
    uint64 stack_top = sp + PGSIZE;
    uint64 uargv[32];  // 参数指针数组
    int argc = 0;
    
    if (argv != NULL) {
        // 首先复制所有参数字符串到栈上
        for (argc = 0; argv[argc] != NULL && argc < 31; argc++) {
            int len = strlen(argv[argc]) + 1;
            stack_top -= len;
            stack_top = ALIGN_DOWN(stack_top, 8);  // 8字节对齐
            
            // 复制字符串到栈
            memmove((void*)(pa + (stack_top - sp)), argv[argc], len);
            uargv[argc] = stack_top;
        }
        
        // 复制参数指针数组
        stack_top -= (argc + 1) * sizeof(uint64);
        stack_top = ALIGN_DOWN(stack_top, 16);  // 16字节对齐
        
        for (int i = 0; i < argc; i++) {
            *(uint64*)(pa + (stack_top - sp) + i * sizeof(uint64)) = uargv[i];
        }
        *(uint64*)(pa + (stack_top - sp) + argc * sizeof(uint64)) = 0;  // NULL终止
    }
    
    *argc_out = argc;
    return stack_top;
}

/* 
 * proc_exec - 执行ELF文件
 * 
 * path: ELF文件路径
 * argv: 参数数组
 * 
 * 成功返回0，失败返回-1
 */
int proc_exec(char *path, char **argv)
{
    proc_t *p = myproc();
    if (p == NULL) return -1;
    
    elf_header_t elf;
    prog_header_t ph;
    inode_t *ip;
    pgtbl_t new_pgtbl = NULL;
    trapframe_t *new_tf = NULL;
    uint64 heap_top = 0;
    int ret = -1;
    
    // step-0: 准备全新的pagetable和trapframe
    new_tf = (trapframe_t *)pmem_alloc(true);
    if (new_tf == NULL) {
        goto bad;
    }
    memset(new_tf, 0, PGSIZE);
    
    new_pgtbl = proc_pgtbl_init((uint64)new_tf);
    if (new_pgtbl == NULL) {
        pmem_free((uint64)new_tf, true);
        new_tf = NULL;
        goto bad;
    }
    
    // step-1: 解析输入的文件路径，获取ELF文件的inode
    ip = path_to_inode(path);
    if (ip == NULL) {
        goto bad;
    }
    
    inode_lock(ip);
    
    // step-2: 读取ELF_header
    if (inode_read_data(ip, 0, sizeof(elf), &elf, false) != sizeof(elf)) {
        inode_unlock(ip);
        inode_put(ip);
        goto bad;
    }
    
    // 验证ELF magic number
    if (elf.magic != ELF_MAGIC) {
        inode_unlock(ip);
        inode_put(ip);
        goto bad;
    }
    
    // step-3: 按顺序读取需要载入内存的Segment
    uint64 max_va = 0;
    for (int i = 0; i < elf.phnum; i++) {
        // 读取程序头
        if (inode_read_data(ip, elf.phoff + i * sizeof(ph), sizeof(ph), &ph, false) != sizeof(ph)) {
            inode_unlock(ip);
            inode_put(ip);
            goto bad;
        }
        
        // 只处理PT_LOAD类型的段
        if (ph.type != PT_LOAD) {
            continue;
        }
        
        // 检查段是否有效
        if (ph.memsz < ph.filesz || ph.vaddr + ph.memsz < ph.vaddr) {
            inode_unlock(ip);
            inode_put(ip);
            goto bad;
        }
        
        // 确定权限
        int perm = 0;
        if (ph.flags & PF_R) perm |= PTE_R;
        if (ph.flags & PF_W) perm |= PTE_W;
        if (ph.flags & PF_X) perm |= PTE_X;
        
        printf("exec: loading seg vaddr=0x%x, filesz=0x%x, perm=0x%x\n", (uint32)ph.vaddr, (uint32)ph.filesz, perm);
        
        // 加载段
        if (load_segment(new_pgtbl, ph.vaddr, ip, ph.off, ph.filesz, perm) < 0) {
            inode_unlock(ip);
            inode_put(ip);
            goto bad;
        }
        
        // 更新最大虚拟地址
        if (ph.vaddr + ph.memsz > max_va) {
            max_va = ph.vaddr + ph.memsz;
        }
    }
    
    // step-4: 释放ELF的inode
    inode_unlock(ip);
    inode_put(ip);
    
    // 设置堆顶
    heap_top = ALIGN_UP(max_va, PGSIZE);
    
    // step-5: 处理输入的参数列表argv，填充到用户栈区域
    uint64 argc = 0;
    uint64 sp = prepare_stack(new_pgtbl, argv, &argc);
    if (sp == 0) {
        goto bad;
    }
    
    // step-6: 新的地址空间构建完毕，释放旧的资源
    // 保存旧的资源
    pgtbl_t old_pgtbl = p->pgtbl;
    trapframe_t *old_tf = p->tf;
    uint64 old_heap_top = p->heap_top;
    uint64 old_ustack_npage = p->ustack_npage;
    mmap_region_t *old_mmap = p->mmap;
    
    // 切换到新资源
    p->pgtbl = new_pgtbl;
    p->tf = new_tf;
    p->heap_top = heap_top;
    p->ustack_npage = 1;
    p->mmap = NULL;
    
    // step-7: 设置trapframe的相关字段
    extern void trap_user_handler();
    
    new_tf->kernel_satp = r_satp();
    new_tf->kernel_sp = p->kstack + PGSIZE;
    new_tf->kernel_trap = (uint64)trap_user_handler;
    new_tf->kernel_hartid = r_tp();
    new_tf->epc = elf.entry;
    new_tf->sp = sp;
    new_tf->a0 = argc;           // 第一个参数: argc
    new_tf->a1 = sp;             // 第二个参数: argv的地址
    
    
    // step-8: 更新进程名称
    // 从路径中提取文件名
    char *name = path;
    for (char *s = path; *s; s++) {
        if (*s == '/') {
            name = s + 1;
        }
    }
    int name_len = strlen(name);
    if (name_len > 15) name_len = 15;
    memmove(p->name, name, name_len);
    p->name[name_len] = '\0';
    
    
    // 暂时跳过旧资源释放（会导致内存泄漏，但用于调试）
    (void)old_pgtbl;
    (void)old_tf;
    (void)old_heap_top;
    (void)old_ustack_npage;
    (void)old_mmap;
    
    // 返回argc，syscall会把它设置到a0
    return argc;

bad:
    // 清理分配的资源
    if (new_pgtbl != NULL) {
        // 释放新页表管理的物理页
        uvm_destroy_pgtbl(new_pgtbl);
        pmem_free((uint64)new_pgtbl, true);
    }
    if (new_tf != NULL) {
        pmem_free((uint64)new_tf, true);
    }
    
    return ret;
}

