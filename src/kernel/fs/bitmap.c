#include "mod.h"

extern super_block_t sb;

/*
 * 在一个位图块（bitmap block）的有效字节范围内，从低位到高位找第一个 0，
 * 置 1 并返回该 bit 的“局部索引”（相对于该位图块起点的 bit 下标）。
 * 若找不到，返回 0xFFFFFFFF。
 */
static uint32 bitmap_search_and_set(uint32 bitmap_block_num, uint32 valid_bytes)
{
    buffer_t *buf = buffer_get(bitmap_block_num);
    uint32 found = 0xFFFFFFFF;

    for (uint32 i = 0; i < valid_bytes; i++) {
        uint8 v = buf->data[i];
        if (v != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if ((v & (1U << bit)) == 0) {
                    v |= (1U << bit);
                    buf->data[i] = v;
                    buffer_write(buf);
                    found = (i * 8u) + (uint32)bit;
                    goto out;
                }
            }
        }
    }

out:
    buffer_put(buf);
    return found;
}

/* 将 bitmap_block_num 中第 index 个 bit 清零 */
static void bitmap_clear(uint32 bitmap_block_num, uint32 index)
{
    buffer_t *buf = buffer_get(bitmap_block_num);

    uint32 byte = index / 8;
    uint8  mask = (uint8)(1U << (index % 8));

    uint8 v = buf->data[byte];
    v &= (uint8)~mask;
    buf->data[byte] = v;
    buffer_write(buf);

    buffer_put(buf);
}

/*==================== 对外接口 ====================*/

uint32 bitmap_alloc_block()
{
    // data 位图从 sb.data_bitmap_firstblock 起，共 sb.data_bitmap_blocks 个位图块
    // 数据块总数由 sb.data_blocks 指定
    uint32 total_data_bits = sb.data_blocks;
    uint32 per_block_bits  = BIT_PER_BLOCK;

    for (uint32 bi = 0; bi < sb.data_bitmap_blocks; bi++) {
        uint32 bitmap_blockno = sb.data_bitmap_firstblock + bi;

        // 剩余可分配 bit 数（决定本位图块有效字节数）
        uint32 used_bits   = bi * per_block_bits;
        uint32 remain_bits = (total_data_bits > used_bits) ? (total_data_bits - used_bits) : 0;
        if (remain_bits == 0) break;

        uint32 valid_bytes = (remain_bits >= per_block_bits) ? BLOCK_SIZE : ((remain_bits + 7) / 8);

        uint32 local = bitmap_search_and_set(bitmap_blockno, valid_bytes);
        if (local != 0xFFFFFFFF) {
            return bi * per_block_bits + local;  // 转为全局 data block 序号（0-based）
        }
    }
    return 0xFFFFFFFF; // 分配失败
}

uint32 bitmap_alloc_inode()
{
    // inode 位图从 sb.inode_bitmap_firstblock 起，共 sb.inode_bitmap_blocks 个位图块
    // inode 总数：sb.total_inodes
    uint32 total_inode_bits = sb.total_inodes;
    uint32 per_block_bits   = BIT_PER_BLOCK;

    for (uint32 bi = 0; bi < sb.inode_bitmap_blocks; bi++) {
        uint32 bitmap_blockno = sb.inode_bitmap_firstblock + bi;

        uint32 used_bits   = bi * per_block_bits;
        uint32 remain_bits = (total_inode_bits > used_bits) ? (total_inode_bits - used_bits) : 0;
        if (remain_bits == 0) break;

        uint32 valid_bytes = (remain_bits >= per_block_bits) ? BLOCK_SIZE : ((remain_bits + 7) / 8);

        uint32 local = bitmap_search_and_set(bitmap_blockno, valid_bytes);
        if (local != 0xFFFFFFFF) {
            return bi * per_block_bits + local;  // 全局 inode 序号（0-based）
        }
    }
    return 0xFFFFFFFF;
}

void bitmap_free_block(uint32 block_num)
{
    if (block_num == 0xFFFFFFFF) return;

    uint32 per_block_bits = BIT_PER_BLOCK;
    uint32 bi    = block_num / per_block_bits;
    uint32 local = block_num % per_block_bits;

    assert(bi < sb.data_bitmap_blocks, "bitmap_free_block: out of range");
    bitmap_clear(sb.data_bitmap_firstblock + bi, local);
}

void bitmap_free_inode(uint32 inode_num)
{
    if (inode_num == 0xFFFFFFFF) return;

    uint32 per_block_bits = BIT_PER_BLOCK;
    uint32 bi    = inode_num / per_block_bits;
    uint32 local = inode_num % per_block_bits;

    assert(bi < sb.inode_bitmap_blocks, "bitmap_free_inode: out of range");
    bitmap_clear(sb.inode_bitmap_firstblock + bi, local);
}

/*
 * 打印 bitmap 的状态：
 *   print_data_bitmap = false → 打印 data bitmap
 *   print_data_bitmap = true  → 打印 inode bitmap
 * 输出格式与课程示例一致。
 */
void bitmap_print(bool print_data_bitmap)
{
    printf("\n[%s bitmap]\n", print_data_bitmap ? "inode" : "data");

    uint32 first_block = print_data_bitmap ? sb.inode_bitmap_firstblock : sb.data_bitmap_firstblock;
    uint32 n_blocks    = print_data_bitmap ? sb.inode_bitmap_blocks     : sb.data_bitmap_blocks;
    uint32 total_bits  = print_data_bitmap ? sb.total_inodes            : sb.data_blocks;

    uint32 printed_bits = 0;

    for (uint32 bi = 0; bi < n_blocks; bi++) {
        buffer_t *buf = buffer_get(first_block + bi);

        uint32 used_bits   = bi * BIT_PER_BLOCK;
        uint32 remain_bits = (total_bits > used_bits) ? (total_bits - used_bits) : 0;
        uint32 valid_bits  = remain_bits >= BIT_PER_BLOCK ? BIT_PER_BLOCK : remain_bits;

        printf("bitmap block %u: ", first_block + bi);

        uint32 bytes = (valid_bits + 7) / 8;
        for (uint32 i = 0; i < bytes; i++) {
            uint8 bytev = buf->data[i];
            for (int bit = 0; bit < 8; bit++) {
                if (printed_bits >= total_bits) break;
                if (bytev & (1U << bit))
                    printf("%u ", bi * BIT_PER_BLOCK + i * 8 + bit);
                printed_bits++;
            }
        }

        buffer_put(buf);
        printf("\n");
    }
    printf("over!\n\n");
}
