#include "mod.h"

/*--------------------part-1: 关于内核空间<->用户空间的数据传递--------------------*/

// 用户态地址空间[src, src+len) 拷贝至 内核态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyin(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 end = src + len;
    while (src < end) {
        uint64 va_page = ALIGN_DOWN(src, PGSIZE);
        uint64 offset = src - va_page;
        uint64 copy_len = MIN(PGSIZE - offset, end - src);
        
        pte_t *pte = vm_getpte(pgtbl, va_page, false);
        assert(pte != NULL && (*pte & PTE_V), "uvm_copyin: invalid user address");
        assert((*pte & PTE_R) || (*pte & PTE_W), "uvm_copyin: no read permission");
        
        uint64 pa = PTE_TO_PA(*pte);
        memmove((void *)(dst), (void *)(pa + offset), copy_len);
        
        src += copy_len;
        dst += copy_len;
    }
}

// 内核态地址空间[src, src+len） 拷贝至 用户态地址空间[dst, dst+len)
// 注意: src dst 不一定是 page-aligned
void uvm_copyout(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 len)
{
    uint64 end = dst + len;
    while (dst < end) {
        uint64 va_page = ALIGN_DOWN(dst, PGSIZE);
        uint64 offset = dst - va_page;
        uint64 copy_len = MIN(PGSIZE - offset, end - dst);
        
        pte_t *pte = vm_getpte(pgtbl, va_page, false);
        assert(pte != NULL && (*pte & PTE_V), "uvm_copyout: invalid user address");
        assert((*pte & PTE_W), "uvm_copyout: no write permission");
        
        uint64 pa = PTE_TO_PA(*pte);
        memmove((void *)(pa + offset), (void *)(src), copy_len);
        
        dst += copy_len;
        src += copy_len;
    }
}

// 用户态字符串拷贝到内核态
// 最多拷贝maxlen字节, 中途遇到'\0'则终止
// 注意: src dst 不一定是 page-aligned
void uvm_copyin_str(pgtbl_t pgtbl, uint64 dst, uint64 src, uint32 maxlen)
{
    uint32 copied = 0;
    uint64 end = src + maxlen;
    
    while (src < end && copied < maxlen) {
        uint64 va_page = ALIGN_DOWN(src, PGSIZE);
        uint64 offset = src - va_page;
        uint64 copy_len = MIN(PGSIZE - offset, end - src);
        
        pte_t *pte = vm_getpte(pgtbl, va_page, false);
        assert(pte != NULL && (*pte & PTE_V), "uvm_copyin_str: invalid user address");
        assert((*pte & PTE_R), "uvm_copyin_str: no read permission");
        
        uint64 pa = PTE_TO_PA(*pte);
        const char *src_ptr = (const char *)(pa + offset);
        
        for (uint32 i = 0; i < copy_len && copied < maxlen; i++) {
            *((char *)dst + copied) = src_ptr[i];
            copied++;
            if (src_ptr[i] == '\0') {
                return;
            }
        }
        
        src += copy_len;
    }
}

/*--------------------part-2: mmap_region相关--------------------*/

// 打印以mmap为首的mmap链
// for debug
void uvm_show_mmaplist(mmap_region_t *mmap) {
    mmap_region_t *tmp = mmap;
    printf("\nalloced mmap_space:\n");
    if (tmp == NULL) {
        printf("empty\n");
        return;
    }
    int count = 0; // 防止无限打印
    while (tmp != NULL && count < 20) { // 限制最大打印次数
        printf("mmap_node: %p | begin: %p ~ %p | next: %p\n",
               tmp, tmp->begin, tmp->begin + tmp->npages * PGSIZE, tmp->next);
        tmp = tmp->next;
        count++;
    }
    if (count >= 20) {
        printf("Warning: mmap list may have cycle!\n");
    }
}

// 提前声明mmap_merge，解决隐式声明问题
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1);

// 两个 mmap_region 区域合并
// 注意: 保留一个 释放一个 不操作 next 指针
// 由uvm_mmap调用
static void mmap_merge(mmap_region_t *mmap_1, mmap_region_t *mmap_2, bool keep_mmap_1)
{
    // 确保有效和紧临
    assert(mmap_1 != NULL && mmap_2 != NULL, "mmap_merge: NULL");
    assert(mmap_1->begin + mmap_1->npages * PGSIZE == mmap_2->begin, "mmap_merge: check fail");

    // merge
    if (keep_mmap_1) {
        mmap_1->npages += mmap_2->npages;
        mmap_region_free(mmap_2);
    } else {
        mmap_2->begin -= mmap_1->npages * PGSIZE;
        mmap_2->npages += mmap_1->npages;
        mmap_region_free(mmap_1);
    }
}

// 寻找一块足够大的区域(len), 作为 mmap_region
// 由uvm_mmap调用(处理begin==0的情况)
// 成功返回begin, 失败返回0
static uint64 uvm_mmap_find(mmap_region_t *head_mmap, uint64 len, mmap_region_t **p_last_mmap, mmap_region_t **p_tmp_mmap)
{
    uint64 required = len;
    *p_last_mmap = NULL;
    *p_tmp_mmap = head_mmap;
    uint64 prev_end = MMAP_BEGIN;

    while (*p_tmp_mmap != NULL) {
        if ((*p_tmp_mmap)->begin - prev_end >= required) {
            return prev_end;
        }
        prev_end = (*p_tmp_mmap)->begin + (*p_tmp_mmap)->npages * PGSIZE;
        *p_last_mmap = *p_tmp_mmap;
        *p_tmp_mmap = (*p_tmp_mmap)->next;
    }

    if (MMAP_END - prev_end >= required) {
        return prev_end;
    }

    return 0;
}

// 在用户页表和进程mmap链里新增mmap区域 [begin, begin + npages * PGSIZE)
// 调用者保证begin是page-aligned的, 页面权限为perm
// 注意: 如果start==0, 意味着需要内核自主找一块足够大的空间
// 失败则panic卡死
void uvm_mmap(uint64 begin, uint32 npages, int perm)
{
    proc_t *p = myproc();
    uint64 len = npages * PGSIZE;
    
    mmap_region_t *last, *tmp;
    mmap_region_t *new_region = mmap_region_alloc();
    if (begin == 0) {
        begin = uvm_mmap_find(p->mmap, len, &last, &tmp);
        assert(begin != 0, "uvm_mmap: no available space");
    } else {
        assert(begin >= MMAP_BEGIN && begin + len <= MMAP_END, "uvm_mmap: invalid address range");
        assert(ALIGN_DOWN(begin, PGSIZE) == begin, "uvm_mmap: address not page-aligned");
        
        tmp = p->mmap;
        while (tmp != NULL) {
            uint64 t_end = tmp->begin + tmp->npages * PGSIZE;
            assert(!(begin < t_end && begin + len > tmp->begin), "uvm_mmap: address conflict");
            tmp = tmp->next;
        }
        last = NULL;
        tmp = p->mmap;
        while (tmp != NULL && tmp->begin < begin) {
            last = tmp;
            tmp = tmp->next;
        }
    }
    
    new_region->begin = begin;
    new_region->npages = npages;
    new_region->next = tmp;
    
    if (last == NULL) {
        p->mmap = new_region;
    } else {
        last->next = new_region;
    }

    for (uint64 va = begin; va < begin + len; va += PGSIZE) {
        uint64 pa = (uint64)pmem_alloc(false);
        assert(pa != 0, "uvm_mmap: failed to alloc page");
        memset((void*)pa, 0, PGSIZE);
        vm_mappages(p->pgtbl, va, pa, PGSIZE, perm | PTE_U | PTE_V);
    }

    if (last != NULL && last->begin + last->npages * PGSIZE == new_region->begin) {
        mmap_merge(last, new_region, true);
        new_region = last; 
        new_region->next = tmp;  //
    }
    
    if (new_region->next != NULL && new_region->begin + new_region->npages * PGSIZE == new_region->next->begin) {
        mmap_merge(new_region, new_region->next, true);
        new_region->next = new_region->next->next; //
    }
}

void uvm_munmap(uint64 begin, uint32 npages)
{
    proc_t *p = myproc();
    uint64 len = npages * PGSIZE;
    uint64 end = begin + len;
    mmap_region_t *prev = NULL, *curr = p->mmap;
    bool freed = false;  

    while (curr != NULL && begin < end) {  
        uint64 curr_end = curr->begin + curr->npages * PGSIZE;
        if (curr_end > begin) {
            freed = true;  
            uint64 unmap_begin = (curr->begin > begin) ? curr->begin : begin;
            uint64 unmap_end = (curr_end < end) ? curr_end : end;
            
            if (begin <= curr->begin && end >= curr_end) {
                vm_unmappages(p->pgtbl, curr->begin, curr->npages * PGSIZE, true);
                
                mmap_region_t *to_free = curr;
                if (prev == NULL) {
                    p->mmap = curr->next;
                    curr = p->mmap;
                } else {
                    prev->next = curr->next;
                    curr = curr->next;
                }
                mmap_region_free(to_free);
                
                if (prev != NULL && curr != NULL && 
                    prev->begin + prev->npages * PGSIZE == curr->begin) {
                    mmap_merge(prev, curr, true);
                    prev->next = curr->next;
                    curr = prev->next;
                }
                begin = curr_end;
                continue;
            }
            else if (begin <= curr->begin && end < curr_end) {
                uint32 unmap_npages = (unmap_end - unmap_begin) / PGSIZE;
                vm_unmappages(p->pgtbl, unmap_begin, unmap_npages * PGSIZE, true);
                
                curr->npages -= unmap_npages;
                curr->begin = unmap_end;
                begin = end;
            }
            else if (begin > curr->begin && end >= curr_end) {
                uint32 unmap_npages = (unmap_end - unmap_begin) / PGSIZE;
                vm_unmappages(p->pgtbl, unmap_begin, unmap_npages * PGSIZE, true);
                
                curr->npages -= unmap_npages;
                begin = curr_end;
            }
            else if (begin > curr->begin && end < curr_end) {
                uint32 unmap_npages = (unmap_end - unmap_begin) / PGSIZE;
                vm_unmappages(p->pgtbl, unmap_begin, unmap_npages * PGSIZE, true);
                
                mmap_region_t *new_mmap = mmap_region_alloc();
                new_mmap->begin = end;
                new_mmap->npages = (curr_end - end) / PGSIZE;
                new_mmap->next = curr->next;
                
                curr->npages = (begin - curr->begin) / PGSIZE;
                curr->next = new_mmap;
                
                mmap_region_t *prev_node = curr;    
                mmap_region_t *curr_node = new_mmap;  
                mmap_region_t *next_node = curr_node->next;  
            
                if (prev_node != NULL && 
                    (prev_node->begin + prev_node->npages * PGSIZE) == curr_node->begin) {
                    prev_node->npages += curr_node->npages;
                    prev_node->next = next_node;
                    mmap_region_free(curr_node);
                    curr_node = prev_node;
                }

                if (next_node != NULL && 
                    (curr_node->begin + curr_node->npages * PGSIZE) == next_node->begin) {
                    curr_node->npages += next_node->npages;
                    curr_node->next = next_node->next;
                    mmap_region_free(next_node);
                }

                prev = new_mmap;
                curr = new_mmap->next;
                begin = end;
                continue;
            }
        }
        
        prev = curr;
        curr = curr->next;
    }

    assert(freed, "uvm_munmap: region not found");
}
/*------------------part-3: 用户空间heap和stack管理相关------------------*/

// 用户堆空间增加, 返回新的堆顶地址 (注意栈顶最大值限制)
uint64 uvm_heap_grow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len) 
{
    if (len == 0) return cur_heap_top;
    uint64 new_top = cur_heap_top + len;
    uint64 mmap_start = MMAP_BEGIN;

    assert(new_top < mmap_start, "uvm_heap_grow: heap exceeds mmap region");

    uint64 va = cur_heap_top;
    while (va < new_top) {
        uint64 page = (uint64)pmem_alloc(false);
        assert(page != 0, "uvm_heap_grow: out of memory");
        memset((void*)page, 0, PGSIZE);
        
        uint64 map_va = ALIGN_UP(va, PGSIZE);
        if (map_va > va) va = map_va;
        
        vm_mappages(pgtbl, va, page, PGSIZE, PTE_R | PTE_W | PTE_U | PTE_V);
        va += PGSIZE;
    }

    return new_top;
}

// 用户堆空间减少, 返回新的堆顶地址
uint64 uvm_heap_ungrow(pgtbl_t pgtbl, uint64 cur_heap_top, uint32 len)
{
    if (len == 0) return cur_heap_top;
    uint64 new_top = cur_heap_top - len;
    assert(new_top >= USER_BASE, "uvm_heap_ungrow: underflow");

    uint64 va = ALIGN_DOWN(new_top, PGSIZE);
    while (va < cur_heap_top) {
        if (va < new_top) {
            va += PGSIZE;
            continue;
        }
        pte_t *pte = vm_getpte(pgtbl, va, false);
        if (pte != NULL && (*pte & PTE_V)) {
            uint64 pa = PTE_TO_PA(*pte);
            vm_unmappages(pgtbl, va, PGSIZE, true);
            pmem_free(pa, false);  // 修正参数类型
        }
        va += PGSIZE;
    }

    return new_top;
}

// 处理函数栈增长导致的page fault事件
// 成功返回new_ustack_npage，失败返回-1
uint64 uvm_ustack_grow(pgtbl_t pgtbl, uint64 old_ustack_npage, uint64 fault_addr)
{
    uint64 stack_bottom = TRAPFRAME - old_ustack_npage * PGSIZE;
    // 移除未使用变量stack_top

    if (fault_addr >= stack_bottom - PGSIZE && fault_addr < stack_bottom) {
        if (old_ustack_npage + 1 > (16 * 1024 * 1024) / PGSIZE) {
            return -1;
        }

        uint64 new_page = (uint64)pmem_alloc(false);
        if (new_page == 0) return -1;
        memset((void*)new_page, 0, PGSIZE);

        uint64 new_va = stack_bottom - PGSIZE;
        // 直接调用，不判断返回值
        vm_mappages(pgtbl, new_va, new_page, PGSIZE, PTE_R | PTE_W | PTE_U | PTE_V);
        // 若需要检查失败，可通过其他方式（如后续访问验证）

        return old_ustack_npage + 1;
    }

    return -1;
}

/*----------------------part-4: 用户页表管理相关----------------------*/

// 递归释放 页表占用的物理页 和 页表管理的物理页
// ps: 顶级页表level = 3
static void destroy_pgtbl(pgtbl_t pgtbl, uint32 level)
{
    if (level == 0) return;

    for (int i = 0; i < 512; i++) {
        pte_t pte = pgtbl[i];
        if (pte & PTE_V && PTE_CHECK(pte)) {
            uint64 child_pa = PTE_TO_PA(pte);
            destroy_pgtbl((pgtbl_t)child_pa, level - 1);
            pmem_free(child_pa, true);  // 内核页表，使用true标记
            pgtbl[i] = 0;
        }
    }
}

// 页表销毁
void uvm_destroy_pgtbl(pgtbl_t pgtbl)
{
    vm_unmappages(pgtbl, TRAPFRAME, PGSIZE, true);
    vm_unmappages(pgtbl, TRAMPOLINE, PGSIZE, false);
    destroy_pgtbl(pgtbl, 3);
}

// 连续虚拟空间的复制
// 在uvm_copy_pgtbl中使用
__attribute__((unused))
static void copy_range(pgtbl_t old, pgtbl_t new, uint64 begin, uint64 end)
{
    uint64 va, pa, page;
    int flags;
    pte_t *pte;

    for (va = begin; va < end; va += PGSIZE)
    {
        pte = vm_getpte(old, va, false);
        assert(pte != NULL, "uvm_copy_pgtbl: pte == NULL");
        assert((*pte) & PTE_V, "uvm_copy_pgtbl: pte not valid");

        pa = (uint64)PTE_TO_PA(*pte);
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
    // -------------------------- 1. 复制堆区域（USER_BASE ~ heap_top）--------------------------
    uint64 va_start = USER_BASE;
    uint64 valid_va_begin = 0;  // 记录当前有效范围的起始VA
    bool in_valid_range = false;

    for (uint64 va = va_start; va < heap_top; va += PGSIZE) {
        pte_t *pte = vm_getpte(old, va, false);
        // 检查PTE是否存在且有效
        bool is_valid = (pte != NULL) && (*pte & PTE_V);

        if (is_valid && !in_valid_range) {
            // 进入有效范围：记录起始VA
            valid_va_begin = va;
            in_valid_range = true;
        } else if (!is_valid && in_valid_range) {
            // 退出有效范围：复制当前有效范围
            copy_range(old, new, valid_va_begin, va);
            in_valid_range = false;
        }
    }
    // 处理末尾的有效范围（如果循环结束时仍在有效范围中）
    if (in_valid_range) {
        copy_range(old, new, valid_va_begin, heap_top);
    }

    // -------------------------- 2. 复制用户栈区域（stack_bottom ~ TRAPFRAME）--------------------------
    uint64 stack_bottom = TRAPFRAME - ustack_npage * PGSIZE;
    in_valid_range = false;  // 重置状态

    for (uint64 va = stack_bottom; va < TRAPFRAME; va += PGSIZE) {
        pte_t *pte = vm_getpte(old, va, false);
        bool is_valid = (pte != NULL) && (*pte & PTE_V);

        if (is_valid && !in_valid_range) {
            valid_va_begin = va;
            in_valid_range = true;
        } else if (!is_valid && in_valid_range) {
            copy_range(old, new, valid_va_begin, va);
            in_valid_range = false;
        }
    }
    if (in_valid_range) {
        copy_range(old, new, valid_va_begin, TRAPFRAME);
    }

    // -------------------------- 3. 复制mmap区域 --------------------------
    mmap_region_t *tmp = mmap;
    while (tmp != NULL) {
        uint64 begin = tmp->begin;
        uint64 end = begin + tmp->npages * PGSIZE;
        in_valid_range = false;  // 重置状态

        for (uint64 va = begin; va < end; va += PGSIZE) {
            pte_t *pte = vm_getpte(old, va, false);
            bool is_valid = (pte != NULL) && (*pte & PTE_V);

            if (is_valid && !in_valid_range) {
                valid_va_begin = va;
                in_valid_range = true;
            } else if (!is_valid && in_valid_range) {
                copy_range(old, new, valid_va_begin, va);
                in_valid_range = false;
            }
        }
        // 处理mmap区域末尾的有效范围
        if (in_valid_range) {
            copy_range(old, new, valid_va_begin, end);
        }

        tmp = tmp->next;
    }

    // -------------------------- 4. 复制trapframe（原有逻辑不变）--------------------------
    pte_t *old_pte = vm_getpte(old, TRAPFRAME, false);
    assert(old_pte != NULL && (*old_pte & PTE_V), "uvm_copy_pgtbl: trapframe not found");
    uint64 trapframe_pa = PTE_TO_PA(*old_pte);
    uint64 new_trapframe_pa = (uint64)pmem_alloc(false);
    assert(new_trapframe_pa != 0, "uvm_copy_pgtbl: alloc trapframe failed");
    memmove((void*)new_trapframe_pa, (void*)trapframe_pa, PGSIZE);
    vm_mappages(new, TRAPFRAME, new_trapframe_pa, PGSIZE, PTE_FLAGS(*old_pte) | PTE_V);

    // -------------------------- 5. 映射trampoline（原有逻辑不变）--------------------------
    old_pte = vm_getpte(old, TRAMPOLINE, false);
    assert(old_pte != NULL && (*old_pte & PTE_V), "uvm_copy_pgtbl: trampoline not found");
    vm_mappages(new, TRAMPOLINE, PTE_TO_PA(*old_pte), PGSIZE, PTE_FLAGS(*old_pte) | PTE_V);
}