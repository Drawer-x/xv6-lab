#!/bin/bash

# Lab7 测试脚本 - 只切换测试代码
# 用法: ./run_test.sh [1|2|3]

TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
USER_DIR="$TEST_DIR/src/user"

write_test1() {
cat > "$USER_DIR/initcode.c" << 'EOF'
// test-1: read superblock
#include "sys.h"

int main()
{
	syscall(SYS_print_str, "hello, world!\n");
	while(1);
}
EOF
}

write_test2() {
cat > "$USER_DIR/initcode.c" << 'EOF'
// test-2: bitmap
#include "sys.h"

#define NUM 20
#define N_BUFFER 8

int main()
{
	unsigned int block_num[NUM];
	unsigned int inode_num[NUM];

	for (int i = 0; i < NUM; i++)
		block_num[i] = syscall(SYS_alloc_block);

	syscall(SYS_flush_buffer, N_BUFFER);
	syscall(SYS_show_bitmap, 0);

	for (int i = 0; i < NUM; i+=2)
		syscall(SYS_free_block, block_num[i]);
	
	syscall(SYS_flush_buffer, N_BUFFER);
	syscall(SYS_show_bitmap, 0);

	for (int i = 1; i < NUM; i+=2)
		syscall(SYS_free_block, block_num[i]);

	syscall(SYS_flush_buffer, N_BUFFER);
	syscall(SYS_show_bitmap, 0);

	for (int i = 0; i < NUM; i++)
		inode_num[i] = syscall(SYS_alloc_inode);

	syscall(SYS_flush_buffer, N_BUFFER);
	syscall(SYS_show_bitmap, 1);

	for (int i = 0; i < NUM; i++)
		syscall(SYS_free_inode, inode_num[i]);

	syscall(SYS_flush_buffer, N_BUFFER);
	syscall(SYS_show_bitmap, 1);

	while(1);
}
EOF
}

write_test3() {
cat > "$USER_DIR/initcode.c" << 'EOF'
#include "sys.h"

#define PGSIZE 4096
#define N_BUFFER 8
#define BLOCK_BASE 5000

int main()
{
	char data[PGSIZE], tmp[PGSIZE];
	unsigned long long buffer[N_BUFFER];

	/*-------------一阶段测试: READ WRITE------------- */

	/* 准备字符串"ABCDEFGH" */
	for (int i = 0; i < 8; i++)
		data[i] = 'A' + i;
	data[8] = '\n';
	data[9] = '\0';

	/* 查看此时的buffer_cache状态 */
	syscall(SYS_print_str, "\nstate-1 ");
	syscall(SYS_show_buffer);

	/* 向BLOCK_BASE写入字符 */
	buffer[0] = syscall(SYS_get_block, BLOCK_BASE);
	syscall(SYS_write_block, buffer[0], data);
	syscall(SYS_put_block, buffer[0]);

	/* 查看此时的buffer_cache状态 */
	syscall(SYS_print_str, "\nstate-2 ");
	syscall(SYS_show_buffer);

	/* 清空内存副本, 确保后面从磁盘中重新读取 */
	syscall(SYS_flush_buffer, N_BUFFER);

	/* 读取BLOCK_BASE*/
	buffer[0] = syscall(SYS_get_block, BLOCK_BASE);
	syscall(SYS_read_block, buffer[0], tmp);
	syscall(SYS_put_block, buffer[0]);

	/* 比较写入的字符串和读到的字符串 */
	syscall(SYS_print_str, "\n");
	syscall(SYS_print_str, "write data: ");
	syscall(SYS_print_str, data);
	syscall(SYS_print_str, "read data: ");
	syscall(SYS_print_str, tmp);

	/* 查看此时的buffer_cache状态 */
	syscall(SYS_print_str, "\nstate-3 ");
	syscall(SYS_show_buffer);

	/*-------------二阶段测试: GET PUT FLUSH------------- */
	
	/* GET */
	buffer[0] = syscall(SYS_get_block, BLOCK_BASE);
	buffer[3] = syscall(SYS_get_block, BLOCK_BASE + 3);
	buffer[7] = syscall(SYS_get_block, BLOCK_BASE + 7);
	buffer[2] = syscall(SYS_get_block, BLOCK_BASE + 2);
	buffer[4] = syscall(SYS_get_block, BLOCK_BASE + 4);

	/* 查看此时的buffer_cache状态 */
	syscall(SYS_print_str, "\nstate-4 ");
	syscall(SYS_show_buffer);

	/* PUT */
	syscall(SYS_put_block, buffer[7]);
	syscall(SYS_put_block, buffer[0]);
	syscall(SYS_put_block, buffer[4]);

	/* 查看此时的buffer_cache状态 */
	syscall(SYS_print_str, "\nstate-5 ");
	syscall(SYS_show_buffer);

	/* FLUSH */
	syscall(SYS_flush_buffer, 3);

	/* 查看此时的buffer_cache状态 */
	syscall(SYS_print_str, "\nstate-6 ");
	syscall(SYS_show_buffer);

	while(1);
}
EOF
}

if [ -z "$1" ]; then
    echo "用法: $0 [1|2|3|clean]"
    echo "  1     - Test-1: 读取 superblock"
    echo "  2     - Test-2: bitmap 分配/释放"
    echo "  3     - Test-3: Buffer LRU 测试"
    echo "  clean - 清理编译产物 (释放磁盘空间)"
    exit 1
fi

case $1 in
    1) write_test1; test_name="Test-1: 读取 superblock" ;;
    2) write_test2; test_name="Test-2: bitmap 分配/释放" ;;
    3) write_test3; test_name="Test-3: Buffer LRU 测试" ;;
    clean|c)
        cd "$TEST_DIR"
        pkill -9 qemu 2>/dev/null || true
        echo ">>> 清理前: $(du -sh target 2>/dev/null | cut -f1)"
        make clean
        echo ">>> 清理完成"
        exit 0
        ;;
    *) echo "无效选项: $1"; exit 1 ;;
esac

cd "$TEST_DIR"

# 杀掉残留的 QEMU
pkill -9 qemu 2>/dev/null || true

# 清理需要重新编译的文件
rm -f target/user/initcode.o target/kernel/proc/proc.o target/mkfs/disk.img 2>/dev/null || true

echo ">>> 已切换到 $test_name"
echo ">>> 运行命令: make run"
