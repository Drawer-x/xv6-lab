#include "mod.h"

// mmap_region_node_t 仓库(单向链表) + 链表头节点(不可分配) + 保护仓库的自旋锁
static mmap_region_node_t node_list[N_MMAP];
static mmap_region_node_t list_head;
static spinlock_t list_lk;

// 初始化仓库、链表头节点和自旋锁
void mmap_init() {
    // 初始化自旋锁
    spinlock_init(&list_lk, "mmap_list_lock");
    
    // 初始化链表头节点
    list_head.next = NULL;
    list_head.mmap.next = NULL;  // 初始化内部mmap的next指针
    
    // 将所有节点加入空闲链表（按顺序0~255）
    spinlock_acquire(&list_lk);
    mmap_region_node_t *last = &list_head;
    for (int i = 0; i < N_MMAP; i++) {
        node_list[i].next = NULL;
        node_list[i].mmap.next = NULL;  // 初始化节点内mmap的next
        last->next = &node_list[i];
        last = &node_list[i];
    }
    spinlock_release(&list_lk);
}

// 从仓库申请一个mmap_region_t
mmap_region_t *mmap_region_alloc() {
    spinlock_acquire(&list_lk);
    
    if (list_head.next == NULL) {
        spinlock_release(&list_lk);
        panic("mmap_region_alloc: no available nodes");
    }
    
    // 从链表头部取出节点
    mmap_region_node_t *node = list_head.next;
    list_head.next = node->next;
    
    spinlock_release(&list_lk);

    return &node->mmap;
}

// 向仓库归还一个mmap_region_t
void mmap_region_free(mmap_region_t *mmap) {
    // 计算包含mmap的节点指针（不使用offsetof，通过指针运算）
    mmap_region_node_t *node = (mmap_region_node_t*)mmap;
    // 调整指针：mmap是node的第一个成员，地址相同
    if ((void*)&node->mmap != (void*)mmap) {
        panic("mmap_region_free: invalid pointer");
    }
    
    spinlock_acquire(&list_lk);
    // 插入到链表头部
    node->next = list_head.next;
    list_head.next = node;
    spinlock_release(&list_lk);
}

// 输出可用的mmap_region_node_t链（调试用）
void mmap_show_nodelist() {
    spinlock_acquire(&list_lk);

    mmap_region_node_t *tmp = list_head.next;
    int node = 0, index = 0;
    while (tmp) {
        index = tmp - &node_list[0];
        printf("node %d index = %d\n", node++, index);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}