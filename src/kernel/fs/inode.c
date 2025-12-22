#include "mod.h"

extern super_block_t sb;

/* 内存中的inode资源集合 */
static inode_t inode_cache[N_INODE];
static spinlock_t lk_inode_cache;

uint32 inode_block(uint32 inode_num)
{
    return sb.inode_firstblock
         + inode_num / INODE_PER_BLOCK;
}
static uint32 locate_block(uint32 *inode_index, uint32 logical_block_num)
{
    uint32 idx1 = logical_block_num;

    /* 直接映射 */
    if (idx1 < INODE_INDEX_1)
        return inode_index[idx1];

    idx1 -= INODE_INDEX_1;
    uint32 per = BLOCK_SIZE / sizeof(uint32);

    /* 一级间接 */
    if (idx1 < (INODE_INDEX_2 - INODE_INDEX_1) * per) {
        uint32 first = idx1 / per;
        uint32 second = idx1 % per;
        uint32 blk = inode_index[INODE_INDEX_1 + first];
        if (blk == 0) return 0;

        buffer_t *buf = buffer_get(blk);
        uint32 *index = (uint32 *)buf->data;
        uint32 res = index[second];
        buffer_put(buf);
        return res;
    }

    idx1 -= (INODE_INDEX_2 - INODE_INDEX_1) * per;

    /* 二级间接 */
    uint32 first = idx1 / (per * per);
    uint32 rest = idx1 % (per * per);
    uint32 second = rest / per;
    uint32 third = rest % per;

    uint32 blk1 = inode_index[INODE_INDEX_2 + first];
    if (blk1 == 0) return 0;

    buffer_t *buf1 = buffer_get(blk1);
    uint32 *idx_lv1 = (uint32 *)buf1->data;
    uint32 blk2 = idx_lv1[second];
    buffer_put(buf1);
    if (blk2 == 0) return 0;

    buffer_t *buf2 = buffer_get(blk2);
    uint32 *idx_lv2 = (uint32 *)buf2->data;
    uint32 res = idx_lv2[third];
    buffer_put(buf2);
    return res;
}
uint32 inode_offset(uint32 inode_num)
{
    return inode_num % INODE_PER_BLOCK;
}
void inode_init()
{
	spinlock_init(&lk_inode_cache, "inode_cache");

	for (int i = 0; i < N_INODE; i++) {
		inode_cache[i].ref = 0;
		inode_cache[i].valid_info = false;
		sleeplock_init(&inode_cache[i].slk, "inode");
	}
}

/*--------------------关于inode->index的增删查操作-----------------*/

/* 
	供free_data_blocks使用
	递归删除inode->index中的一个元素
	返回删除过程中是否遇到空的block_num (文件末尾)
*/
static bool __free_data_blocks(uint32 block_num, uint32 level)
{
	if (block_num == 0)
		return true;

	if (level == 0) {
		bitmap_free_block(block_num);
		return false;
	}

	buffer_t *buf = buffer_get(block_num);
	uint32 *index = (uint32 *)buf->data;

	for (int i = 0; i < BLOCK_SIZE / sizeof(uint32); i++) {
		if (__free_data_blocks(index[i], level - 1)) {
			buffer_put(buf);
			bitmap_free_block(block_num);
			return true;
		}
	}

	buffer_put(buf);
	bitmap_free_block(block_num);
	return false;
}


/* 
	释放inode管理的blocks
*/
static void free_data_blocks(uint32 *inode_index)
{
	unsigned int i;
	bool meet_empty = false;

	/* step-1: 释放直接映射的block */
	for (i = 0; i < INODE_INDEX_1; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 0);
		if (meet_empty) return;
	}

	/* step-2: 释放一级间接映射的block */
	for (; i < INODE_INDEX_2; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 1);
		if (meet_empty) return;
	}

	/* step-3: 释放二级间接映射的block */
	for (; i < INODE_INDEX_3; i++)
	{
		meet_empty = __free_data_blocks(inode_index[i], 2);
		if (meet_empty) return;		
	}

	panic("free_data_blocks: impossible!");
}

/*
	获取inode第logical_block_num个block的物理序号block_num
	调用者保证输入的logical_block_num只有两种情况:
	1. 属于已经分配的区域 (返回block_num)
	2. 将已经分配出去的区域往外扩展1个block (申请block并返回block_num) 
	成功返回block_num, 失败返回-1
*/
#define IDX_PER_BLOCK (BLOCK_SIZE / sizeof(uint32))
uint32 locate_or_add_block(uint32 *index, uint32 lbn)
{
    uint32 block;

    /* =========================================================
     * 0️⃣ 直接映射
     * ========================================================= */
    if (lbn < INODE_INDEX_1) {
        if (index[lbn] == 0) {
            block = bitmap_alloc_block();
            if (block == 0)
                panic("locate_or_add_block: no data block");

            index[lbn] = block;
        }
        return index[lbn];
    }

    lbn -= INODE_INDEX_1;

    /* =========================================================
     * 1️⃣ 一级间接映射
     * ========================================================= */
    uint32 l1_cap = (INODE_INDEX_2 - INODE_INDEX_1) * IDX_PER_BLOCK;

    if (lbn < l1_cap) {
        uint32 i1 = lbn / IDX_PER_BLOCK;
        uint32 i2 = lbn % IDX_PER_BLOCK;

        /* 分配一级索引块 */
        if (index[INODE_INDEX_1 + i1] == 0) {
            block = bitmap_alloc_block();
            if (block == 0)
                panic("locate_or_add_block: no index block");

            index[INODE_INDEX_1 + i1] = block;

            buffer_t *b = buffer_get(block);
            memset(b->data, 0, BLOCK_SIZE);
            buffer_write(b);
            buffer_put(b);
        }

        buffer_t *b = buffer_get(index[INODE_INDEX_1 + i1]);
        uint32 *idx = (uint32 *)b->data;

        if (idx[i2] == 0) {
            block = bitmap_alloc_block();
            if (block == 0) {
                buffer_put(b);
                panic("locate_or_add_block: no data block");
            }

            idx[i2] = block;
            buffer_write(b);
        }

        block = idx[i2];
        buffer_put(b);
        return block;
    }

    lbn -= l1_cap;

    /* =========================================================
     * 2️⃣ 二级间接映射
     * ========================================================= */
    uint32 l2_cap =
        (INODE_INDEX_3 - INODE_INDEX_2) *
        IDX_PER_BLOCK * IDX_PER_BLOCK;

    if (lbn >= l2_cap)
        panic("locate_or_add_block: file too large");

    uint32 i1 = lbn / (IDX_PER_BLOCK * IDX_PER_BLOCK);
    uint32 rest = lbn % (IDX_PER_BLOCK * IDX_PER_BLOCK);
    uint32 i2 = rest / IDX_PER_BLOCK;
    uint32 i3 = rest % IDX_PER_BLOCK;

    if (i1 >= (INODE_INDEX_3 - INODE_INDEX_2))
        panic("locate_or_add_block: index overflow");

    /* -------- 一级索引块 -------- */
    if (index[INODE_INDEX_2 + i1] == 0) {
        block = bitmap_alloc_block();
        if (block == 0)
            panic("locate_or_add_block: no index_index block");

        index[INODE_INDEX_2 + i1] = block;

        buffer_t *b = buffer_get(block);
        memset(b->data, 0, BLOCK_SIZE);
        buffer_write(b);
        buffer_put(b);
    }

    buffer_t *b1 = buffer_get(index[INODE_INDEX_2 + i1]);
    uint32 *idx1 = (uint32 *)b1->data;

    /* -------- 二级索引块 -------- */
    if (idx1[i2] == 0) {
        block = bitmap_alloc_block();
        if (block == 0) {
            buffer_put(b1);
            panic("locate_or_add_block: no index block");
        }

        idx1[i2] = block;

        buffer_t *btmp = buffer_get(block);
        memset(btmp->data, 0, BLOCK_SIZE);
        buffer_write(btmp);
        buffer_put(btmp);

        buffer_write(b1);
    }

    uint32 data_index_block = idx1[i2];
    buffer_put(b1);

    buffer_t *b2 = buffer_get(data_index_block);
    uint32 *idx2 = (uint32 *)b2->data;

    if (idx2[i3] == 0) {
        block = bitmap_alloc_block();
        if (block == 0) {
            buffer_put(b2);
            panic("locate_or_add_block: no data block");
        }

        idx2[i3] = block;
        buffer_write(b2);
    }

    block = idx2[i3];
    buffer_put(b2);
    return block;
}



/*---------------------关于inode的管理: get dup lock unlock put----------------------*/

/* 
	磁盘里的inode <-> 内存里的inode
	调用者需要持有ip->slk并设置合理的inode_num
*/
void inode_rw(inode_t *ip, bool write)
{
	buffer_t *buf = buffer_get(inode_block(ip->inode_num));
	inode_disk_t *dip =
	(inode_disk_t *)buf->data + inode_offset(ip->inode_num);

	if (write)
		*dip = ip->disk_info;
	else
		ip->disk_info = *dip;

	buffer_write(buf);
	buffer_put(buf);
	ip->valid_info = true;
}


/*
	尝试在inode_cache里寻找是否存在目标inode
	如果不存在则申请一个空闲的inode
	如果没有空闲位置直接panic
	核心逻辑: ref++
*/
inode_t *inode_get(uint32 inode_num)
{
	spinlock_acquire(&lk_inode_cache);

	inode_t *empty = NULL;

	for (int i = 0; i < N_INODE; i++) {
		if (inode_cache[i].ref > 0 &&
		    inode_cache[i].inode_num == inode_num) {
			inode_cache[i].ref++;
			spinlock_release(&lk_inode_cache);
			return &inode_cache[i];
		}
		if (empty == NULL && inode_cache[i].ref == 0)
			empty = &inode_cache[i];
	}

	assert(empty != NULL, "inode_get: no free inode");

	empty->ref = 1;
	empty->inode_num = inode_num;
	empty->valid_info = false;

	spinlock_release(&lk_inode_cache);
	return empty;
}


/*
	在磁盘里创建1个新的inode
	1. 查询和修改inode_bitmap
	2. 填充inode_region对应位置的inode
	注意: 返回的inode未上锁
*/
inode_t *inode_create(uint16 type, uint16 major, uint16 minor)
{
	uint32 inode_num = bitmap_alloc_inode();
	if (inode_num == 0)
		return NULL;

	inode_t *ip = inode_get(inode_num);
	inode_lock(ip);

    memset(&ip->disk_info, 0, sizeof(ip->disk_info));
	ip->disk_info.type = type;
	ip->disk_info.major = major;
	ip->disk_info.minor = minor;
	ip->disk_info.nlink = 1;
	ip->disk_info.size = 0;

	inode_rw(ip, true);
	inode_unlock(ip);

	return ip;
}


/*
	ip->ref++ with lock proctect
*/
inode_t* inode_dup(inode_t* ip)
{
	spinlock_acquire(&lk_inode_cache);
	ip->ref++;
	spinlock_release(&lk_inode_cache);
	return ip;
}


/*
	锁住inode
	如果inode->disk_info无效则更新一波
*/
void inode_lock(inode_t* ip)
{
	sleeplock_acquire(&ip->slk);
	if (!ip->valid_info)
		inode_rw(ip, false);
}

void inode_unlock(inode_t *ip)
{
	sleeplock_release(&ip->slk);
}


/*
	与inode_get相对应, 调用者释放inode资源
	如果达成某些条件, 可能触发彻底删除
*/
void inode_put(inode_t *ip)
{
    spinlock_acquire(&lk_inode_cache);

    ip->ref--;

    if (ip->ref == 0 && ip->disk_info.nlink == 0) {
        spinlock_release(&lk_inode_cache);

        inode_lock(ip);
        inode_delete(ip);
        inode_unlock(ip);

        return;
    }

    spinlock_release(&lk_inode_cache);
}

/*
	在磁盘里删除1个inode
	1. 修改inode_bitmap释放inode_region资源
	2. 修改block_bitmap释放block_region资源
	注意: 调用者需要持有ip->slk
*/
void inode_delete(inode_t *ip)
{
    assert(sleeplock_holding(&ip->slk), "inode_delete: slk");

    /* 1. 释放数据 blocks */
    free_data_blocks(ip->disk_info.index);

    /* 2. 清 inode 内容 */
    memset(&ip->disk_info, 0, sizeof(ip->disk_info));
    ip->valid_info = false;

    /* 3. 写回磁盘 */
    inode_rw(ip, true);

    /* 4. 释放 inode bitmap */
    bitmap_free_inode(ip->inode_num);
}

/*----------------------基于inode的数据读写操作--------------------*/

/*
	基于inode的数据读取
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝data[offset,offset+len)到dst(用户态地址/内核态地址)
	返回读取的数据量(字节)
*/
uint32 inode_read_data(inode_t *ip, uint32 offset, uint32 len, void *dst, bool is_user_dst)
{
    if (offset >= ip->disk_info.size)
        return 0;

    if (offset + len > ip->disk_info.size)
        len = ip->disk_info.size - offset;

    uint32 done = 0;

    while (done < len) {
        uint32 block_no = (offset + done) / BLOCK_SIZE;
        uint32 block_off = (offset + done) % BLOCK_SIZE;
        uint32 cnt = MIN(len - done, BLOCK_SIZE - block_off);

        uint32 bnum = locate_block(ip->disk_info.index, block_no);
        if (bnum == 0)
            break;

        buffer_t *buf = buffer_get(bnum);

        if (is_user_dst) {
            proc_t *p = myproc();
            if (!p) panic("inode_read_data: no proc");
            uvm_copyout(p->pgtbl,
                        (uint64)(dst + done),
                        (uint64)(buf->data + block_off),
                        cnt);
        } else {
            memmove(dst + done, buf->data + block_off, cnt);
        }

        buffer_put(buf);
        done += cnt;
    }

    return done;
}




/*
	基于inode的数据写入
	inode管理的数据空间逻辑上是一个连续的数组data
	需要拷贝src(用户态地址/内核态地址)到data[offset,offset+len)
	返回写入的数据量(字节)
*/
uint32 inode_write_data(inode_t *ip,
                        uint32 offset,
                        uint32 len,
                        void *src,
                        bool user)
{
    uint32 tot = 0;
    uint32 block_no, block_offset;
    uint32 n;

    uint32 max_blocks =
        INODE_INDEX_1 +
        (INODE_INDEX_2 - INODE_INDEX_1) * IDX_PER_BLOCK +
        (INODE_INDEX_3 - INODE_INDEX_2) * IDX_PER_BLOCK * IDX_PER_BLOCK;

    uint32 max_size = max_blocks * BLOCK_SIZE;

    if (offset + len > max_size)
        printf("inode_write_data: file too large");

    while (tot < len) {
        block_no = (offset + tot) / BLOCK_SIZE;
        block_offset = (offset + tot) % BLOCK_SIZE;

        if (block_no >= max_blocks)
            printf("inode_write_data: block_no overflow");

        uint32 bnum = locate_or_add_block(ip->disk_info.index, block_no);
        if (bnum == (uint32)-1)
            printf("inode_write_data: no block");

        buffer_t *buf = buffer_get(bnum);

        n = BLOCK_SIZE - block_offset;
        if (n > len - tot)
            n = len - tot;

        if (n == 0)
            printf("inode_write_data: n == 0");

        memcpy(buf->data + block_offset,
               (char *)src + tot,
               n);

        buffer_write(buf);
        buffer_put(buf);

        tot += n;
    }

    if (offset + len > ip->disk_info.size)
        ip->disk_info.size = offset + len;

    //inode_rw(ip, true);
    return len;
}


static char *inode_type_list[] = {"DATA", "DIR", "DEVICE"};

/* 输出inode信息(for debug) */
void inode_print(inode_t *ip, char* name)
{
	assert(sleeplock_holding(&ip->slk), "inode_print: slk");

	spinlock_acquire(&lk_inode_cache);

	printf("inode %s:\n", name);
	printf("ref = %d, inode_num = %d, valid_info = %d\n", ip->ref, ip->inode_num, ip->valid_info);
	printf("type = %s, major = %d, minor = %d, nlink = %d, size = %d\n", inode_type_list[ip->disk_info.type],
		ip->disk_info.major, ip->disk_info.minor, ip->disk_info.nlink, ip->disk_info.size);

	printf("index_list = [ ");
	for (int i = 0; i < INODE_INDEX_1; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_1; i < INODE_INDEX_2; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("] [ ");
	for (int i = INODE_INDEX_2; i < INODE_INDEX_3; i++)
		printf("%d ", ip->disk_info.index[i]);
	printf("]\n\n");

	spinlock_release(&lk_inode_cache);
}