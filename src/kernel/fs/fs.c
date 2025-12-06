#include "mod.h"
#include "../lib/method.h"   // 使用内核自带的 memcpy/memset/memmove/strncmp 声明

super_block_t sb;

/* 打印 superblock（便于 test-1 验证布局信息） */
static void sb_print()
{
    printf("\n[superblock]\n");
    printf("magic: 0x%x\n", sb.magic_num);
    printf("block_size: %u\n", sb.block_size);
    printf("total_blocks: %u\n", sb.total_blocks);
    printf("total_inodes: %u\n", sb.total_inodes);

    printf("inode_bitmap_firstblock: %u\n", sb.inode_bitmap_firstblock);
    printf("inode_bitmap_blocks: %u\n", sb.inode_bitmap_blocks);
    printf("inode_firstblock: %u\n", sb.inode_firstblock);
    printf("inode_blocks: %u\n", sb.inode_blocks);

    printf("data_bitmap_firstblock: %u\n", sb.data_bitmap_firstblock);
    printf("data_bitmap_blocks: %u\n", sb.data_bitmap_blocks);
    printf("data_firstblock: %u\n", sb.data_firstblock);
    printf("data_blocks: %u\n", sb.data_blocks);

    printf("\n");
}

/* 文件系统初始化：buffer 初始化 + 读入 superblock */
void fs_init()
{
    buffer_init();

    buffer_t *b = buffer_get(FS_SB_BLOCK);   // 超级块通常在块 0
    memmove(&sb, b->data, sizeof(super_block_t));
    buffer_put(b);

    sb_print();
}
