// src/kernel/mem/mmap.c
#include "mod.h"

// mmap_region_node_t 仓库(单向链表) + 链表头节点(不可分配) + 保护仓库的自旋锁
static mmap_region_node_t node_list[N_MMAP];
static mmap_region_node_t list_head;
static spinlock_t list_lk;

// 初始化上述三个数据结构
void mmap_init()
{
    // 1) 初始化锁
    spinlock_init(&list_lk, "mmap_node_list");

    // 2) 头节点不可分配，只作为链表哨兵
    list_head.next = NULL;

    // 3) 把 node_list 里的 0~N_MMAP-1 都串到头后面
    //    这里顺序不重要，只要是单链表就行
    for (int i = 0; i < N_MMAP; i++)
    {
        node_list[i].mmap.begin  = 0;
        node_list[i].mmap.npages = 0;
        node_list[i].mmap.next   = NULL;
        node_list[i].next = list_head.next;
        list_head.next    = &node_list[i];
    }
}

// 从仓库申请一个 mmap_region_t
// 若仓库空了则 panic
mmap_region_t *mmap_region_alloc()
{
    spinlock_acquire(&list_lk);

    mmap_region_node_t *node = list_head.next;
    if (node == NULL)
    {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: no free node");
    }

    // 摘下这个节点
    list_head.next = node->next;

    // 清空一下内容，返回里面真正的 mmap 部分
    node->mmap.begin  = 0;
    node->mmap.npages = 0;
    node->mmap.next   = NULL;

    spinlock_release(&list_lk);

    return &node->mmap;
}

/* 释放mmap_region_t节点到仓库 */
void mmap_region_free(mmap_region_t *mmap) {
    if (!mmap) return;  // 忽略空指针new

    // 从mmap指针反推节点指针（依赖结构体布局）
    mmap_region_node_t *node = (mmap_region_node_t *)mmap;
    spinlock_acquire(&list_lk);

    // 插入链表头部
    node->next = list_head.next;
    list_head.next = node;
    spinlock_release(&list_lk);
}

// 打印当前仓库里节点的情况（按实验给的格式）
void mmap_show_nodelist()
{
    spinlock_acquire(&list_lk);

    mmap_region_node_t *tmp = list_head.next;
    int node = 0, index = 0;
    while (tmp)
    {
        index = tmp - &(node_list[0]);
        printf("node %d index = %d\n", node++, index);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}
