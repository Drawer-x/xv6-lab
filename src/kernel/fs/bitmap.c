#include "mod.h"

extern super_block_t sb;

/*
 * 在一个位图块（bitmap block）的有效bit范围内，从低位到高位找第一个 0，
 * 置 1 并返回该 bit 的"局部索引"（相对于该位图块起点的 bit 下标）。
 * valid_count: 该block中有效的bit数量（最后一个block可能只用了一部分）
 * 若找不到，返回 0xFFFFFFFF。
 */
static uint32 bitmap_search_and_set(uint32 bitmap_block_num, uint32 valid_count)
{
    buffer_t *buf = buffer_get(bitmap_block_num);
    uint32 bit_index = 0xFFFFFFFF;
    uint32 current_bit = 0;

    // 逐字节遍历
    for (uint32 byte = 0; byte < BLOCK_SIZE && current_bit < valid_count; byte++) {
        // 如果该字节不全为1，则有空闲bit
        if (buf->data[byte] != 0xFF) {
            // 逐bit遍历该字节
            for (uint32 shift = 0; shift < BIT_PER_BYTE && current_bit < valid_count; shift++) {
                uint8 mask = (uint8)(1U << shift);
                if ((buf->data[byte] & mask) == 0) {
                    // 找到空闲bit，设置为1
                    buf->data[byte] |= mask;
                    bit_index = current_bit;
                    buffer_write(buf);
                    buffer_put(buf);
                    return bit_index;
                }
                current_bit++;
            }
        } else {
            current_bit += BIT_PER_BYTE;
        }
    }

    buffer_put(buf);
    return 0xFFFFFFFF; // 没有找到空闲bit
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
    uint32 bits_scanned = 0;

    for (uint32 i = 0; i < sb.data_bitmap_blocks; i++) {
        uint32 bitmap_block_num = sb.data_bitmap_firstblock + i;

        // 计算当前block中有效的bit数量
        uint32 remaining_bits = sb.data_blocks - bits_scanned;
        uint32 valid_count = (remaining_bits > BIT_PER_BLOCK) ? BIT_PER_BLOCK : remaining_bits;

        // 在该block中搜索并设置
        uint32 bit_index = bitmap_search_and_set(bitmap_block_num, valid_count);
        if (bit_index != 0xFFFFFFFF) {
            // 找到了，返回全局序号
            return sb.data_firstblock + bits_scanned + bit_index;
        }

        bits_scanned += BIT_PER_BLOCK;
    }

    panic("bitmap_alloc_block: no free block");
    return 0xFFFFFFFF; // 分配失败
}

uint32 bitmap_alloc_inode()
{
    // inode 位图从 sb.inode_bitmap_firstblock 起，共 sb.inode_bitmap_blocks 个位图块
    // inode 总数：sb.total_inodes
    uint32 bits_scanned = 0;

    for (uint32 i = 0; i < sb.inode_bitmap_blocks; i++) {
        uint32 bitmap_block_num = sb.inode_bitmap_firstblock + i;

        // 计算当前block中有效的bit数量
        uint32 remaining_bits = sb.total_inodes - bits_scanned;
        uint32 valid_count = (remaining_bits > BIT_PER_BLOCK) ? BIT_PER_BLOCK : remaining_bits;

        // 在该block中搜索并设置
        uint32 bit_index = bitmap_search_and_set(bitmap_block_num, valid_count);
        if (bit_index != 0xFFFFFFFF) {
            // 找到了，返回全局序号（inode序号从0开始）
            return bits_scanned + bit_index;
        }

        bits_scanned += BIT_PER_BLOCK;
    }

    panic("bitmap_alloc_inode: no free inode");
    return 0xFFFFFFFF;
}

void bitmap_free_block(uint32 block_num)
{
    if (block_num == 0xFFFFFFFF) return;

    // 计算该block在data_bitmap中的相对位置
    uint32 relative_index = block_num - sb.data_firstblock;

    // 计算该bit所在的bitmap block
    uint32 bitmap_block_num = sb.data_bitmap_firstblock + relative_index / BIT_PER_BLOCK;

    // 计算该bit在bitmap block中的位置
    uint32 bit_index = relative_index % BIT_PER_BLOCK;

    // 清除该bit
    bitmap_clear(bitmap_block_num, bit_index);
}

void bitmap_free_inode(uint32 inode_num)
{
    if (inode_num == 0xFFFFFFFF) return;

    // 计算该inode所在的bitmap block
    uint32 bitmap_block_num = sb.inode_bitmap_firstblock + inode_num / BIT_PER_BLOCK;

    // 计算该bit在bitmap block中的位置
    uint32 bit_index = inode_num % BIT_PER_BLOCK;

    // 清除该bit
    bitmap_clear(bitmap_block_num, bit_index);
}

/*
 * 打印 bitmap 的状态：
 *   print_data_bitmap = false → 打印 data bitmap
 *   print_data_bitmap = true  → 打印 inode bitmap
 * 输出格式与课程示例一致。
 */
void bitmap_print(bool print_inode_bitmap)
{
    // 标题行
    if (print_inode_bitmap)
        printf("\ninode bitmap allocated bits:\n");
    else
        printf("\ndata bitmap allocated bits:\n");

    uint32 first_block = print_inode_bitmap ? sb.inode_bitmap_firstblock : sb.data_bitmap_firstblock;
    uint32 n_blocks    = print_inode_bitmap ? sb.inode_bitmap_blocks     : sb.data_bitmap_blocks;
    uint32 total_bits  = print_inode_bitmap ? sb.total_inodes            : sb.data_blocks;

    // data 需要打印“绝对块号”，inode 打“相对编号”
    uint32 base_offset = print_inode_bitmap ? 0 : sb.data_firstblock;

    uint32 printed_bits = 0;
    for (uint32 bi = 0; bi < n_blocks && printed_bits < total_bits; bi++) {
        buffer_t *buf = buffer_get(first_block + bi);

        uint32 used_bits   = bi * BIT_PER_BLOCK;
        uint32 remain_bits = (total_bits > used_bits) ? (total_bits - used_bits) : 0;
        uint32 valid_bits  = remain_bits >= BIT_PER_BLOCK ? BIT_PER_BLOCK : remain_bits;

        uint32 bytes = (valid_bits + 7) / 8;
        for (uint32 i = 0; i < bytes && printed_bits < total_bits; i++) {
            uint8 bytev = buf->data[i];
            if (bytev == 0) { // 这个字节全 0，直接跳过能省点输出判断
                printed_bits += 8;
                continue;
            }
            for (int bit = 0; bit < 8; bit++) {
                uint32 global_bit = bi * BIT_PER_BLOCK + i * 8 + bit;
                if (global_bit >= total_bits) break;
                if (bytev & (1U << bit)) {
                    // data: 绝对块号；inode: 相对编号
                    printf("%u ", base_offset + global_bit);
                }
                printed_bits++;
            }
        }

        buffer_put(buf);
    }

    printf("over!\n\n");
}
