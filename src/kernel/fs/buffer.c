#include "mod.h"
#include <stddef.h>           // 为 offsetof
#include "../mem/method.h"    // 声明 pmem_alloc / pmem_free
#include "../proc/method.h"   // 声明 proc_sleep / proc_wakeup / myproc 等


static buffer_node_t buf_cache[N_BUFFER];
static buffer_node_t buf_head_active, buf_head_inactive;
static spinlock_t lk_buf_cache;

/* 
 * 工具：将一个节点从原位置摘下，并插入目标链表（头/尾）
 * insert_active=true  表示插入活跃链表，否则插入非活跃链表
 * insert_next=true    表示插入到 head->next（表头），否则 head->prev（表尾）
 */
static void insert_node(buffer_node_t *node, bool insert_active, bool insert_next)
{
    // 1) 从原位置摘除
    if (node->next && node->prev) {
        node->prev->next = node->next;
        node->next->prev = node->prev;
    }

    buffer_node_t *head = insert_active ? &buf_head_active : &buf_head_inactive;

    if (insert_next) {
        // 头部： head <-> head->next
        node->next = head->next;
        node->prev = head;
        head->next->prev = node;
        head->next = node;
    } else {
        // 尾部： head->prev <-> head
        node->next = head;
        node->prev = head->prev;
        head->prev->next = node;
        head->prev = node;
    }
}

/* 初始化：全部放入非活跃链表，ref=0，block_num=UNUSED，data=NULL，睡眠锁初始化 */
void buffer_init()
{
    spinlock_init(&lk_buf_cache, "buf_cache");

    // init heads (双向循环)
    buf_head_active.next = buf_head_active.prev = &buf_head_active;
    buf_head_inactive.next = buf_head_inactive.prev = &buf_head_inactive;

    for (int i = 0; i < N_BUFFER; i++) {
        buffer_node_t *n = &buf_cache[i];
        n->buf.block_num = BLOCK_NUM_UNUSED;
        n->buf.ref = 0;
        n->buf.data = NULL;
        n->buf.disk = false;
        sleeplock_init(&n->buf.slk, "buffer");

        // 按“最后创建的在表头”的顺序插到非活跃链表头
        n->next = n->prev = NULL;
        insert_node(n, /*active*/false, /*head*/true);
    }
}

/* 根据 block 号在活跃/非活跃链表中查找，命中则将其移动到活跃链表头并 ref++ */
static buffer_node_t* find_in_lists(uint32 blockno)
{
    // 先找活跃
    for (buffer_node_t *p = buf_head_active.next; p != &buf_head_active; p = p->next) {
        if (p->buf.block_num == blockno) {
            insert_node(p, true, true); // 置顶（LRU）
            p->buf.ref++;
            return p;
        }
    }
    // 再找非活跃
    for (buffer_node_t *p = buf_head_inactive.next; p != &buf_head_inactive; p = p->next) {
        if (p->buf.block_num == blockno) {
            // 从非活跃移到活跃表头
            insert_node(p, true, true);
            p->buf.ref++;
            return p;
        }
    }
    return NULL;
}

/* 从系统里拿一个“最不活跃”的节点（非活跃表尾）；必要时分配 data 页面 */
static buffer_node_t* alloc_victim(uint32 blockno)
{
    // 非活跃表为空：只能从活跃表尾“偷”一个（必须是 ref==0 的；正常情况下不会发生）
    buffer_node_t *victim = buf_head_inactive.prev;
    if (victim == &buf_head_inactive) {
        // 尝试从活跃链表尾部找 ref==0 的
        for (buffer_node_t *p = buf_head_active.prev; p != &buf_head_active; p = p->prev) {
            if (p->buf.ref == 0) {
                victim = p;
                break;
            }
        }
        if (victim == &buf_head_inactive)
            panic("buffer: no victim");
    }

    // 绑定新 block，移动到活跃表头
    victim->buf.block_num = blockno;
    victim->buf.ref = 1;
    insert_node(victim, true, true);

    // 分配内存页（懒分配）
    if (victim->buf.data == NULL) {
        victim->buf.data = (uint8 *)pmem_alloc(true);
        assert(victim->buf.data != NULL, "buffer: alloc page");
        memset(victim->buf.data, 0, BLOCK_SIZE);
    }
    return victim;
}

/* 获取一个描述 block 的 buffer；若 miss 则从磁盘读入；返回前持有睡眠锁 */
buffer_t* buffer_get(uint32 block_num)
{
    spinlock_acquire(&lk_buf_cache);

    buffer_node_t *node = find_in_lists(block_num);
    if (node == NULL) {
        node = alloc_victim(block_num);

        // 读入磁盘
        sleeplock_acquire(&node->buf.slk);
        node->buf.disk = true;
        virtio_disk_rw(&node->buf, /*write=*/false);
        while (node->buf.disk)
            proc_sleep(&node->buf, &lk_buf_cache);

        // 此时磁盘中断已唤醒，数据在 data 里
        // 护栏：确保当前 CPU 持有 lk_buf_cache，再释放，避免“跨核释放”
        if (!spinlock_holding(&lk_buf_cache)) {
            spinlock_acquire(&lk_buf_cache);
        }
        spinlock_release(&lk_buf_cache);

        return &node->buf;

    } else {
        // 命中：上锁后返回
        sleeplock_acquire(&node->buf.slk);
        spinlock_release(&lk_buf_cache);
        return &node->buf;
    }
}

/* 释放一个 buffer（调用者已持有睡眠锁）；ref--，为0则移入非活跃表头；最终释放睡眠锁 */
void buffer_put(buffer_t *buf)
{
    spinlock_acquire(&lk_buf_cache);

    buffer_node_t *node = (buffer_node_t *)((char*)buf - offsetof(buffer_node_t, buf));
    assert(node->buf.ref > 0, "buffer_put: ref<=0");

    node->buf.ref--;
    if (node->buf.ref == 0) {
        // 移入非活跃表头（仍然保留 data 作为缓存）
        insert_node(node, /*active*/false, /*head*/true);
    }

    sleeplock_release(&node->buf.slk);
    spinlock_release(&lk_buf_cache);
}

/* 将 buffer 的 data 写回磁盘（调用者必须已持有睡眠锁） */
void buffer_write(buffer_t *buf)
{
    spinlock_acquire(&lk_buf_cache);

    buffer_node_t *node = (buffer_node_t *)((char*)buf - offsetof(buffer_node_t, buf));
    assert(sleeplock_holding(&node->buf.slk), "buffer_write: lock not held");

    node->buf.disk = true;
    virtio_disk_rw(&node->buf, /*write=*/true);
    while (node->buf.disk)
        proc_sleep(&node->buf, &lk_buf_cache);

    // 护栏：确保当前 CPU 持有 lk_buf_cache，再释放
    if (!spinlock_holding(&lk_buf_cache)) {
        spinlock_acquire(&lk_buf_cache);
    }
    spinlock_release(&lk_buf_cache);

}

/* 释放若干个“最不活跃”的 buffer 持有的物理页（仅非活跃表内扫描） */
uint32 buffer_freemem(uint32 buffer_count)
{
    spinlock_acquire(&lk_buf_cache);

    uint32 freed = 0;
    for (buffer_node_t *p = buf_head_inactive.prev; p != &buf_head_inactive && freed < buffer_count; p = p->prev) {
        if (p->buf.ref == 0 && p->buf.data != NULL) {
            pmem_free((uint64)p->buf.data, true);
            p->buf.data = NULL;
            freed++;
        }
    }

    spinlock_release(&lk_buf_cache);
    return freed;
}

/* 打印活跃/非活跃链表状态（用于测试） */
void buffer_print_info()
{
    spinlock_acquire(&lk_buf_cache);

    buffer_node_t *node;

    printf("\n[buffer cache info]\n");
    printf("1.active list:\n");
    for (node = buf_head_active.next; node != &buf_head_active; node = node->next) {
        printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
            (int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
    }
    printf("over!\n");

    printf("2.inactive list:\n");
    for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
        printf("buffer %d(ref = %d): page(pa = %p) -> block[%d]\n",
            (int)(node - buf_cache), node->buf.ref, (uint64)node->buf.data, node->buf.block_num);
    }
    printf("over!\n");

    spinlock_release(&lk_buf_cache);
}
