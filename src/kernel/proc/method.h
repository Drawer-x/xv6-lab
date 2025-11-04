#pragma once

// proc.c: 页表初始化 + 第一个进程初始化
pgtbl_t proc_pgtbl_init(uint64 trapframe);
void proc_make_first();


// 用户地址空间布局
#define USER_CODE_VA    0x0000000000001000  // 代码起始地址
#define USER_HEAP_TOP   0x0000000000009000  // 堆顶（栈底）

