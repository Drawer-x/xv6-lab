#include "mod.h"

/*
 * 物理内存管理：基于空闲页链表
 * 
 * ALLOC_BEGIN ~ ALLOC_END : 可分配的物理页区域
 * 前 KERN_PAGES 属于内核空间，后面属于用户空间
 * 每个 alloc_region_t 记录一段可分配区间
 */

static alloc_region_t kern_region, user_region;

/* 内部工具函数 */
static inline int is_page_aligned(uint64 p) {
    return (p & (PGSIZE - 1)) == 0;
}

static void region_build(alloc_region_t *r, uint64 begin, uint64 end, char *name) {
    if (!is_page_aligned(begin) || !is_page_aligned(end) || begin > end) {
        panic("pmem: bad region");
    }

    r->begin     = begin;
    r->end       = end;
    r->allocable = 0;
    spinlock_init(&r->lk, name);

    // 初始化链表头为空
    r->list_head.next = 0;

    // 把每个物理页挂到链表里
    for (uint64 p = begin; p + PGSIZE <= end; p += PGSIZE) {
        page_node_t *node = (page_node_t*)p;
        node->next = r->list_head.next;
        r->list_head.next = node;
        r->allocable++;
    }
}

/* 初始化物理内存 */
void pmem_init(void) {
    uint64 base   = (uint64)ALLOC_BEGIN;
    uint64 end    = (uint64)ALLOC_END;
    uint64 split  = base + (uint64)KERN_PAGES * PGSIZE;

    region_build(&kern_region, base, split,  "kern_region");
    region_build(&user_region, split, end,   "user_region");
}

/* 分配一个物理页 */
void* pmem_alloc(bool in_kernel) {
    alloc_region_t *r = in_kernel ? &kern_region : &user_region;

    spinlock_acquire(&r->lk);

    page_node_t *node = r->list_head.next;
    if (node == 0) {
        spinlock_release(&r->lk);
        panic("pmem_alloc: no free page");
    }

    r->list_head.next = node->next;
    r->allocable--;

    spinlock_release(&r->lk);

    // 清零页内容
    memset((void*)node, 0, PGSIZE);
    return (void*)node;
}

/* 释放一个物理页 */
void pmem_free(uint64 page, bool in_kernel) {
    if (!is_page_aligned(page)) {
        panic("pmem_free: not aligned");
    }

    alloc_region_t *r = in_kernel ? &kern_region : &user_region;

    if (page < r->begin || page >= r->end) {
        panic("pmem_free: out of range");
    }

    page_node_t *node = (page_node_t*)page;

    spinlock_acquire(&r->lk);
    node->next = r->list_head.next;
    r->list_head.next = node;
    r->allocable++;
    spinlock_release(&r->lk);
}

/* 提供访问接口 */
alloc_region_t* get_user_region(void) {
    return &user_region;
}

alloc_region_t* get_kern_region(void) {
    return &kern_region;
}
