// src/kernel/mem/uvm.c
#include "mod.h"

/*--------------------part-1: 关于内核空间<->用户空间的数据传递--------------------*/

// 一个小工具：从用户页表查出某个用户VA对应的PA
// 查不到就返回0
static inline uint64
uvm_va2pa(pgtbl_t pgtbl, uint64 va)
{
    pte_t *pte = vm_getpte(pgtbl, va, false);
    if (pte == NULL || (*pte & PTE_V) == 0)
        return 0;
    uint64 pa = PTE_TO_PA(*pte);
    pa |= (va & (PGSIZE - 1));    // 补上页内偏移
    return pa;
}

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
// 修改后：返回 0 成功，-1 失败
int uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len) {
    while (len > 0) {
        uint64 pa = uvm_va2pa(pgtbl, src);
        if (pa == 0) { // 地址无效
            return -1;
        }

        uint32 n = PGSIZE - (src & (PGSIZE - 1));
        if (n > len) n = len;

        memmove((void *)dst, (const void *)pa, n);

        dst += n;
        src += n;
        len -= n;
    }
    return 0;
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
// 修改后：返回 0 成功，-1 失败
int uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len) {
    while (len > 0) {
        uint64 pa = uvm_va2pa(pgtbl, dst);
        if (pa == 0) { // 地址无效
            return -1;
        }

        uint32 n = PGSIZE - (dst & (PGSIZE - 1));
        if (n > len) n = len;

        memmove((void *)pa, (const void *)src, n);

        dst += n;
        src += n;
        len -= n;
    }
    return 0;
}

// 用户态地址空间的字符串(src) 拷贝到 内核态(dst)
// 最多拷贝 maxlen 个字节，保证以 '\0' 结束
// 修改后：返回实际复制的字节数（不含终止符），-1 失败
int uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen) {
    uint32 copied = 0;
    while (copied < maxlen) {
        uint64 pa = uvm_va2pa(pgtbl, src);
        if (pa == 0) { // 地址无效
            return -1;
        }

        uint32 n = PGSIZE - (src & (PGSIZE - 1));
        while (n > 0 && copied < maxlen) {
            char c = *(char *)pa;
            *(char *)dst = c;

            dst++;
            pa++;
            src++;
            copied++;

            if (c == '\0') {
                return copied - 1; // 不含终止符
            }

            n--;
        }
    }

    // 强制添加终止符
    ((char *)dst)[-1] = '\0';
    return copied - 1;
}

/*--------------------part-2: 用户态 mmap / munmap 逻辑--------------------*/

// 打印进程当前的 mmap 链表
void uvm_show_mmaplist(mmap_region_t *mmap)
{
    mmap_region_t *tmp = mmap;
    if (tmp == NULL)
        printf("empty\n");
    while (tmp != NULL)
    {
        printf("alloced mmap_region: %p ~ %p\n",
               (void *)tmp->begin,
               (void *)(tmp->begin + tmp->npages * PGSIZE));
        tmp = tmp->next;
    }
}

// 两个 mmap_region 区域合并
// 注意: 保留一个 释放一个 不操作 next 指针
// 由 uvm_mmap 调用
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1)
{
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin,
           "mmap_merge: check fail");

    if (keep_mmap_1)
    {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    }
    else
    {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 处理 begin == 0 的情况：在 [MMAP_BEGIN, MMAP_END) 里找一段没被占的、长度 >= len 的洞
// 返回找到的起始虚拟地址；找不到返回 0
// 为了后面合并方便，这里还把“前一个节点”和“当前节点”带出去
static uint64
uvm_mmap_find(mmap_region_t *head_mmap,
              uint64 len,
              mmap_region_t **p_last_mmap,
              mmap_region_t **p_tmp_mmap)
{
    // 我们想要找的区间长度是 len (字节)，它一定是 page-aligned 的
    // 扫描逻辑：
    //   [MMAP_BEGIN, first->begin)
    //   [this->end, next->begin)
    //   ...
    uint64 cur = MMAP_BEGIN;
    mmap_region_t *last = NULL;
    mmap_region_t *tmp  = head_mmap;

    while (1)
    {
        uint64 next_begin = (tmp == NULL) ? MMAP_END : tmp->begin;
        if (next_begin >= cur && next_begin - cur >= len)
        {
            // 找到了一个足够大的洞
            *p_last_mmap = last;
            *p_tmp_mmap  = tmp;
            return cur;
        }

        if (tmp == NULL)
            break;

        // 下一个洞从当前节点的结尾开始
        cur  = tmp->begin + tmp->npages * PGSIZE;
        last = tmp;
        tmp  = tmp->next;
    }

    return 0;
}

// 在用户页表和进程 mmap 链里新增 mmap 区域 [begin, begin + npages * PGSIZE)
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    proc_t *p = myproc();
    assert(p != NULL, "uvm_mmap: no current proc");

    uint64 len = (uint64)npages * PGSIZE;
    assert(len > 0, "uvm_mmap: len == 0");

    // 1) 如果用户传 begin==0，需要自己找一块
    mmap_region_t *last = NULL;
    mmap_region_t *tmp  = p->mmap;
    if (begin == 0)
    {
        uint64 found = uvm_mmap_find(p->mmap, len, &last, &tmp);
        assert(found != 0, "uvm_mmap: no space in mmap region");
        begin = found;
    }
    else
    {
        // 用户自己传了 begin，要保证它在合法 mmap 区间里
        assert(begin >= MMAP_BEGIN && begin + len <= MMAP_END,
               "uvm_mmap: begin out of mmap range");

        // 我们还是要找到“应该插到哪里”这个位置
        last = NULL;
        tmp  = p->mmap;
        while (tmp && tmp->begin < begin)
        {
            last = tmp;
            tmp  = tmp->next;
        }

        // 要确保这段空间不会和后面的冲突
        if (tmp)
            assert(begin + len <= tmp->begin, "uvm_mmap: overlap with next");
        if (last)
            assert(last->begin + last->npages * PGSIZE <= begin,
                   "uvm_mmap: overlap with prev");
    }

    // 2) 现在可以真正分配一个 mmap_region_t 结构体了
    mmap_region_t *new_mmap = mmap_region_alloc();
    new_mmap->begin  = begin;
    new_mmap->npages = npages;
    new_mmap->next   = tmp;

    // 插到链表里
    if (last == NULL)
        p->mmap = new_mmap;
    else
        last->next = new_mmap;

    // 3) 尝试和前后相邻的合并（节省节点）
    // 和前面合并
    if (last &&
        last->begin + last->npages * PGSIZE == new_mmap->begin)
    {
        mmap_merge(last, new_mmap, true);
        new_mmap = last;          // 合并后 new_mmap 其实就是 last
    }
    // 和后面合并
    if (new_mmap->next &&
        new_mmap->begin + new_mmap->npages * PGSIZE == new_mmap->next->begin)
    {
        mmap_merge(new_mmap, new_mmap->next, true);
    }

    // 4) 建立真正的页表映射
    for (uint32 i = 0; i < npages; i++)
    {
        void *page = pmem_alloc(false);
        assert(page != NULL, "uvm_mmap: pmem_alloc failed");
        vm_mappages(p->pgtbl,
                    begin + i * PGSIZE,
                    (uint64)page,
                    PGSIZE,
                    perm | PTE_U);
    }
}

// 在用户页表和进程 mmap 链里释放 mmap 区域 [begin, begin + npages * PGSIZE)
// 失败则 panic 卡死
void uvm_munmap(uint64 begin, uint32 npages)
{
    proc_t *p = myproc();
    assert(p != NULL, "uvm_munmap: no current proc");

    uint64 len = (uint64)npages * PGSIZE;
    assert(begin >= MMAP_BEGIN && begin + len <= MMAP_END,
           "uvm_munmap: range out of mmap");

    mmap_region_t *prev = NULL;
    mmap_region_t *cur  = p->mmap;

    // 找到覆盖这个区间的节点
    while (cur && !(cur->begin <= begin &&
                    begin + len <= cur->begin + cur->npages * PGSIZE))
    {
        prev = cur;
        cur  = cur->next;
    }

    assert(cur != NULL, "uvm_munmap: region not found");

    // 1) 先把页表里的映射解掉并释放物理页
    vm_unmappages(p->pgtbl, begin, len, true);

    // 2) 再看这个节点要不要拆分
    uint64 cur_begin = cur->begin;
    uint64 cur_end   = cur->begin + cur->npages * PGSIZE;

    uint64 unmap_begin = begin;
    uint64 unmap_end   = begin + len;

    // 情况1：整段都删
    if (unmap_begin == cur_begin && unmap_end == cur_end)
    {
        if (prev)
            prev->next = cur->next;
        else
            p->mmap = cur->next;
        mmap_region_free(cur);
    }
    // 情况2：删掉前面一段（从节点起点开始删）
    else if (unmap_begin == cur_begin)
    {
        cur->begin  = unmap_end;
        cur->npages = (cur_end - unmap_end) / PGSIZE;
    }
    // 情况3：删掉后面一段（到节点末尾）
    else if (unmap_end == cur_end)
    {
        cur->npages = (unmap_begin - cur_begin) / PGSIZE;
    }
    // 情况4：中间挖一个洞 -> 要新建一个节点出来
    else
    {
        // 原节点保留前半段
        uint32 front_npages = (unmap_begin - cur_begin) / PGSIZE;
        uint32 back_npages  = (cur_end - unmap_end) / PGSIZE;

        cur->npages = front_npages;

        mmap_region_t *new_mmap = mmap_region_alloc();
        new_mmap->begin  = unmap_end;
        new_mmap->npages = back_npages;
        new_mmap->next   = cur->next;
        cur->next        = new_mmap;
    }
}

/*------------------part-3: 用户空间 heap 和 stack 管理相关------------------*/

// 用户堆空间增加, 返回新的堆顶地址 (注意不能越过 MMAP_BEGIN)
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    if (len == 0)
        return cur_heap_top;

    uint64 old_top = cur_heap_top;
    uint64 new_top = cur_heap_top + len;

    // 不能往上撞到 mmap 区域
    if (new_top > MMAP_BEGIN)
        return (uint64)-1;

    // 逐页分配并建立映射
    uint64 va = ALIGN_UP(old_top, PGSIZE);
    while (va < new_top)
    {
        void *page = pmem_alloc(false);
        assert(page != NULL, "uvm_heap_grow: pmem_alloc failed");
        vm_mappages(pgtbl, va, (uint64)page, PGSIZE, PTE_R | PTE_W | PTE_U);
        va += PGSIZE;
    }

    return new_top;
}

// 用户堆空间减少, 返回新的堆顶地址
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    if (len == 0)
        return cur_heap_top;

    uint64 old_top = cur_heap_top;
    uint64 new_top = cur_heap_top - len;

    // 从 new_top 向上到 old_top 把页都解掉
    uint64 va = ALIGN_UP(new_top, PGSIZE);
    uint64 end = ALIGN_UP(old_top, PGSIZE);
    if (end > va)
        vm_unmappages(pgtbl, va, end - va, true);

    return new_top;
}

// 处理函数栈增长导致的 page fault 事件
// 成功返回 new_ustack_npage，失败返回 -1
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    // 栈顶固定是 TRAPFRAME，栈往下长
    // 当前已经分配的栈底地址是：
    uint64 cur_stack_bot = TRAPFRAME - old_ustack_npage * PGSIZE;

    // fault_addr 必须落在 “已分配的下面、但在 MMAP_END 之上”的区域
    if (fault_addr >= TRAPFRAME)
        return (uint64)-1;

    // 对齐到页
    uint64 need_bot = ALIGN_DOWN(fault_addr, PGSIZE);

    // 不能越过 mmap 区域
    if (need_bot < MMAP_END)
        return (uint64)-1;

    // 需要补多少页
    uint64 need_npage = (TRAPFRAME - need_bot) / PGSIZE;
    if (need_npage <= old_ustack_npage)
        return old_ustack_npage; // 其实已经有了

    // 一个一个补
    for (uint64 va = cur_stack_bot - PGSIZE; va >= need_bot; va -= PGSIZE)
    {
        void *page = pmem_alloc(false);
        assert(page != NULL, "uvm_ustack_grow: pmem_alloc failed");
        vm_mappages(pgtbl, va, (uint64)page, PGSIZE, PTE_R | PTE_W | PTE_U);
        if (va == need_bot)
            break; // 防止无符号循环
    }

    return need_npage;
}

/*----------------------part-4: 用户页表管理相关----------------------*/

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表 level = 3
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    for (int i = 0; i < 512; i++)
    {
        pte_t pte = pgtbl[i];
        if (pte & PTE_V)
        {
            uint64 pa = PTE_TO_PA(pte);
            if (level > 1)
            {
                // 这是一个中间页表
                destroy_pgtbl((pgtbl_t)pa, level - 1);
            }
            else
            {
                // 这是叶子页，直接释放
                pmem_free(pa, false);
            }
        }
    }
    // 最后一层也要把自己释放掉
    pmem_free((uint64)pgtbl, false);
}

void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    destroy_pgtbl(pgtbl, 3);
}

// 拷贝区间 [begin, end) 的所有用户页 (old -> new)
void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t *pte;

    for (va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");

        pa    = (uint64)PTE_TO_PA(*pte);
        flags = (int)PTE_FLAGS(*pte);

        page = (uint64)pmem_alloc(false);
        memmove((char *)page, (const char *)pa, PGSIZE);
        vm_mappages(new, va, page, PGSIZE, flags);
    }
}

// 拷贝页表 (拷贝并不包括 trapframe 和 trampoline)
// 拷贝的页表管理的物理页是原来页表的复制品
void uvm_copy_pgtbl(pgtbl_t old, pgtbl_t new, uint64 heap_top, uint64 ustack_npage, mmap_region_t *mmap)
{
    // 1) 用户代码 + 数据 + 堆以下的所有页
    if (heap_top > USER_BASE)
        copy_range(old, new, USER_BASE, ALIGN_UP(heap_top, PGSIZE));

    // 2) 栈
    if (ustack_npage > 0)
    {
        uint64 stack_bot = TRAPFRAME - ustack_npage * PGSIZE;
        copy_range(old, new, stack_bot, TRAPFRAME);
    }

    // 3) mmap 链
    mmap_region_t *tmp = mmap;
    while (tmp)
    {
        uint64 begin = tmp->begin;
        uint64 end   = tmp->begin + tmp->npages * PGSIZE;
        copy_range(old, new, begin, end);
        tmp = tmp->next;
    }
}
