#include "mod.h"
#include "../lib/method.h"   // 使用内核自带的 memcpy/memset/memmove/strncmp 声明

super_block_t sb;

/* ===== 覆盖 sb_print()：严格匹配 test-1 的版式 ===== */
static void sb_print(void)
{
    uint32 ibmap_begin = sb.inode_bitmap_firstblock;
    uint32 ibmap_end   = sb.inode_bitmap_firstblock + sb.inode_bitmap_blocks - 1;

    uint32 inode_begin = sb.inode_firstblock;
    uint32 inode_end   = sb.inode_firstblock + sb.inode_blocks - 1;

    uint32 dbmap_begin = sb.data_bitmap_firstblock;
    uint32 dbmap_end   = sb.data_bitmap_firstblock + sb.data_bitmap_blocks - 1;

    uint32 data_begin  = sb.data_firstblock;
    uint32 data_end    = sb.data_firstblock + sb.data_blocks - 1;

    uint64 total_mb = ((uint64)sb.total_blocks * (uint64)sb.block_size) / (1024ull * 1024ull);

    printf("\n");
    printf("disk layout information:\n");
    printf("1. super block:  block[0]\n");
    printf("2. inode bitmap: block[%u - %u]\n", ibmap_begin, ibmap_end);
    printf("3. inode region: block[%u - %u]\n", inode_begin, inode_end);
    printf("4. data bitmap:  block[%u - %u]\n", dbmap_begin, dbmap_end);
    printf("5. data region:  block[%u - %u]\n", data_begin, data_end);
    printf("block size = %u Byte, total size = %u MB, total inode = %u\n",
           sb.block_size, (uint32)total_mb, sb.total_inodes);
    printf("\n");
}

/* ===== 覆盖 fs_init()：去掉调试前后缀，仅初始化并打印布局 ===== */
void fs_init(void)
{
    buffer_init();

    buffer_t *b = buffer_get(FS_SB_BLOCK);   // 超级块在 block 0
    memmove(&sb, b->data, sizeof(super_block_t));
    buffer_put(b);

    sb_print();
}
