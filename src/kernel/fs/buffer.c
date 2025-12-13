#include "mod.h"
#include "../mem/method.h"

static buffer_node_t buf_cache[N_BUFFER];
static buffer_node_t buf_head_active, buf_head_inactive;
static spinlock_t lk_buf_cache;

/* 
	将一个节点拿出来并插入
	1. 活跃链表的头部 buf_head_active->next
	2. 活跃链表的尾部 buf_head_active->prev
	3. 不活跃链表的头部 buf_head_inactive->next
	4. 不活跃链表的尾部 buf_head_inactive->prev
 */
static void insert_node(buffer_node_t *node, bool insert_active, bool insert_next)
{
	/* 如果有需要, 让node先离开当前位置 */
	if (node->next != NULL && node->prev != NULL) {
		node->next->prev = node->prev;
        node->prev->next = node->next;
    }

	/* 选择目标双向循环链表 */
	buffer_node_t *head = &buf_head_inactive;
	if (insert_active)
		head = &buf_head_active;

	/* 然后将node插入head->next or head->prev */	
    if (insert_next) {
        node->next = head->next;
		node->next->prev = node;
        node->prev = head;
        head->next = node;
    } else {
		node->prev = head->prev;
		node->prev->next = node;
        node->next = head;
        head->prev = node;
    }
}

/* 
	buffer系统初始化：
	1. 初始化全局的lk_buf_cache + buf_head_active + buf_head_inactive
	2. 初始化buf_cache中的所有node, 并将他们放在不活跃链表中
*/
void buffer_init()
{
	// 初始化全局自旋锁
    spinlock_init(&lk_buf_cache, "buf_cache");

	// 初始化活跃链表头节点 (双向循环链表)
	buf_head_active.next = &buf_head_active;
	buf_head_active.prev = &buf_head_active;

	// 初始化不活跃链表头节点 (双向循环链表)
	buf_head_inactive.next = &buf_head_inactive;
	buf_head_inactive.prev = &buf_head_inactive;

	// 初始化buf_cache中的所有buffer并插入不活跃链表
	// 倒序插入，使得第一个buffer最后位于buf_head_inactive->next
	for (int i = N_BUFFER - 1; i >= 0; i--) {
		buf_cache[i].buf.block_num = BLOCK_NUM_UNUSED;
		buf_cache[i].buf.ref = 0;
		buf_cache[i].buf.data = NULL;
		buf_cache[i].buf.disk = false;
		sleeplock_init(&buf_cache[i].buf.slk, "buffer");
		buf_cache[i].next = NULL;
		buf_cache[i].prev = NULL;
		insert_node(&buf_cache[i], false, true); // 插入不活跃链表头部
    }
}

/* 磁盘读取: block -> buf */
static void buffer_read(buffer_t *buf)
{
	// 确保调用者持有睡眠锁
	assert(sleeplock_holding(&buf->slk), "buffer_read: not holding sleeplock");
	virtio_disk_rw(buf, false); // false表示读操作
}

/* 磁盘写入: buf -> block */
void buffer_write(buffer_t *buf)
{
	// 确保调用者持有睡眠锁
	assert(sleeplock_holding(&buf->slk), "buffer_write: not holding sleeplock");
	virtio_disk_rw(buf, true); // true表示写操作
}

/* 从buf_cache中获取一个buf */
buffer_t* buffer_get(uint32 block_num)
{
	buffer_node_t *node;
	bool cache_hit = false;

    spinlock_acquire(&lk_buf_cache);

	// 1. 首先在活跃链表中寻找
	for (node = buf_head_active.next; node != &buf_head_active; node = node->next) {
		if (node->buf.block_num == block_num) {
			// 找到了，增加引用计数，移动到活跃链表头部
			node->buf.ref++;
			insert_node(node, true, true); // 移动到活跃链表的head->next
			cache_hit = true;
			break;
		}
	}

	// 2. 如果没找到，在不活跃链表中寻找
	if (!cache_hit) {
		for (node = buf_head_inactive.next; node != &buf_head_inactive; node = node->next) {
			if (node->buf.block_num == block_num) {
				// 找到了，增加引用计数，移动到活跃链表头部
				node->buf.ref++;
				insert_node(node, true, true); // 移动到活跃链表的head->next
				cache_hit = true;
				break;
			}
		}
	}

	// 3. 如果还是没找到，说明缓存失败，需要从不活跃链表中获取一个最不活跃的buffer
	if (!cache_hit) {
		// 从不活跃链表尾部（最不活跃）获取一个buffer
		node = buf_head_inactive.prev;
		if (node == &buf_head_inactive) {
			// 不活跃链表为空，无法分配
			panic("buffer_get: no available buffer");
		}
		
		// 设置新的block_num，增加引用计数，移动到活跃链表尾部
		node->buf.block_num = block_num;
		node->buf.ref = 1;
		insert_node(node, true, false); // 移动到活跃链表的head->prev (尾部)
	}

        spinlock_release(&lk_buf_cache);

	// 获取睡眠锁
        sleeplock_acquire(&node->buf.slk);

	// 如果是缓存失败，需要为buffer分配物理内存并从磁盘读入数据
	if (!cache_hit) {
		// 自动申请物理内存（如果需要）
        if (node->buf.data == NULL) {
			node->buf.data = (uint8*)pmem_alloc(true);
		}
		// 从磁盘读入数据
		buffer_read(&node->buf);
            }

        return &node->buf;
}

/* 向buf_cache归还一个buf */
void buffer_put(buffer_t *buf)
{
	// 释放睡眠锁
	sleeplock_release(&buf->slk);

	// 获取自旋锁保护ref
    spinlock_acquire(&lk_buf_cache);

	// 获取buffer对应的node
	buffer_node_t *node = (buffer_node_t*)buf; // buffer_t是buffer_node_t的第一个成员

	// 减少引用计数
    node->buf.ref--;

	// 如果引用计数减到0，移动到不活跃链表的头部
    if (node->buf.ref == 0) {
		insert_node(node, false, true); // 移动到不活跃链表的head->next
    }

    spinlock_release(&lk_buf_cache);
}

/*
	从后向前遍历非活跃链表, 尝试释放buffer_count个buffer持有的物理内存(data)
	返回成功释放资源的buffer数量
*/
uint32 buffer_freemem(uint32 buffer_count)
{
	uint32 freed = 0;
	buffer_node_t *node;

    spinlock_acquire(&lk_buf_cache);

	// 从不活跃链表尾部开始遍历（最不活跃的）
	for (node = buf_head_inactive.prev; 
	     node != &buf_head_inactive && freed < buffer_count; 
	     node = node->prev) {
		// 尝试释放物理内存
		if (node->buf.data != NULL) {
			pmem_free((uint64)node->buf.data, true);
			node->buf.data = NULL;
			node->buf.block_num = BLOCK_NUM_UNUSED; // 清除block_num以避免缓存命中
            freed++;
        }
    }

    spinlock_release(&lk_buf_cache);

    return freed;
}

/* 输出buffer_cache的信息 (for test) */
void buffer_print_info()
{
	buffer_node_t *node;

	assert(N_BUFFER == N_BUFFER_TEST, "buffer_print_info: invalid N_BUFFER");

    spinlock_acquire(&lk_buf_cache);

	printf("buffer_cache information:\n");

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
