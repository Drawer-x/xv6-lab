
---

# LAB-7 README  
## 文件系统 · 磁盘管理（Disk Management）

---

## 一、实验过程性日志（按实验任务推进）

本次 Lab-7 的目标是：**围绕磁盘管理构建文件系统的底层基础设施**。实验整体按照“磁盘 → block 读写 → buffer → bitmap → 系统调用 → 用户测试”的路径逐步完成。

---

### 1. disk.img 的构建与磁盘引入（mkfs）

实验首先通过 **mkfs 工具**构建初始磁盘映像 `disk.img`，该步骤在 **Linux 用户态**完成，而非运行在我们实现的内核中。

- 相关文件：
  - `src/mkfs/mkfs.c`
  - `src/mkfs/mkfs.h`

磁盘被抽象为以 **block（BLOCK_SIZE = PAGE_SIZE）** 为单位的线性数组，整体布局为：

```

[ superblock | inode bitmap | inode region | data bitmap | data region ]

````

初始化完成后：

- superblock 写入磁盘
- inode bitmap 与 data bitmap 全部清零
- inode region / data region 仅作为预留空间存在

这一步为后续内核中的磁盘访问提供了**确定的物理布局基础**。

---

### 2. block-level 磁盘读写能力的建立（virtio + trap）

#### （1）磁盘驱动（virtio）

- 相关文件：
  - `src/kernel/fs/virtio.c`

驱动提供以下核心接口：

```c
void virtio_disk_init();
void virtio_disk_rw(buffer_t *b, bool write);
void virtio_disk_intr();
````

其职责是：

* 以 **block 为单位**与磁盘交互
* 使用 **中断方式**完成 I/O
* 为 buffer 子系统提供底层读写能力

---

#### （2）OS 与磁盘驱动的配合（TODO 实现部分）

为使 virtio 驱动真正可用，本实验在多个模块中完成了必要配合：

1. **内存系统（kvm.c）**

   * 在内核页表中映射磁盘相关 MMIO 寄存器
   * 修改 `vm_getpte`，支持传入 `NULL` 时使用内核页表

2. **中断系统（plic.c）**

   * 使能 virtio block 设备中断
   * 设置磁盘中断优先级

3. **陷阱处理（trap_kernel.c）**

   * 在外设中断分支中识别磁盘中断
   * 调用磁盘中断处理函数，唤醒等待 I/O 的进程

这一阶段完成后，内核具备了**异步 block-level 磁盘读写能力**。

---

### 3. 建立磁盘与内存的数据交换桥梁 —— buffer 系统

* 相关文件：

  * `src/kernel/fs/buffer.c`

buffer 是 **磁盘 block 在内存中的缓存表示**，其核心结构为：

```c
typedef struct buffer {
    uint32 block_num;     // 磁盘 block 编号
    uint32 ref;           // 引用计数
    sleeplock_t slk;      // 睡眠锁
    uint8* data;          // block 数据
    bool disk;            // 供 virtio 使用
} buffer_t;
```

#### buffer 系统完成的功能：

1. **资源初始化（buffer_init）**

   * 所有 buffer 初始进入非活跃链表
   * `ref = 0`，`block_num = BLOCK_NUM_UNUSED`

2. **资源获取（buffer_get）**

   * 优先在活跃链表查找（LRU）
   * 其次在非活跃链表查找
   * 若缓存未命中，回收最不活跃 buffer
   * buffer 被获取后 `ref++`

3. **资源释放（buffer_put）**

   * `ref--`
   * 当 `ref == 0` 时移动到非活跃链表

4. **磁盘读写（buffer_read / buffer_write）**

   * 底层统一调用 `virtio_disk_rw`
   * 调用者必须持有 buffer 的睡眠锁

5. **物理内存回收（buffer_freemem）**

   * 从非活跃链表尾部释放物理页

buffer 系统是 **文件系统访问磁盘的唯一入口**。

---

### 4. 使用 buffer 读入 superblock（fs 初始化）

* 相关文件：

  * `src/kernel/fs/fs.c`
  * `src/kernel/proc/proc.c`

superblock 的读取**不能在内核初始化阶段完成**，因为磁盘 I/O 会触发 `proc_sleep`。

正确的时机是：

> **proczero 第一次进入 `proc_return` 时**

在此位置调用 `fs_init`：

* 初始化 buffer 系统
* 通过 buffer 读入 superblock
* 输出磁盘布局信息（`sb_print`）

---

### 5. bitmap 管理磁盘资源

* 相关文件：

  * `src/kernel/fs/bitmap.c`

基于 buffer，实现以下接口：

```c
uint32 bitmap_alloc_block();
uint32 bitmap_alloc_inode();
void bitmap_free_block(uint32 block_num);
void bitmap_free_inode(uint32 inode_num);
```

其核心逻辑包括：

* 在 bitmap block 中逐 bit 查找空闲位
* bitmap 区域可能跨多个 block
* 释放时将对应 bit 清零
* 所有 bitmap 读写均通过 buffer 完成

---

### 6. 新增系统调用（供用户态测试）

* 相关文件：

  * `src/kernel/syscall/syscall.c`
  * `src/kernel/syscall/sysfunc.c`

共新增 **11 个系统调用**，用于测试 bitmap 与 buffer 功能，例如：

```c
#define SYS_alloc_block   11
#define SYS_free_block    12
#define SYS_show_bitmap   15
#define SYS_get_block     16
#define SYS_write_block   18
#define SYS_show_buffer   20
#define SYS_flush_buffer  21
```

系统调用主要完成：

* 参数解析
* 调用对应内核实现函数
* 将结果返回用户态

---

## 二、相比上一个实验新增的功能

与上一个实验相比，Lab-7 新增并首次完整打通了：

1. **磁盘外设（virtio block）支持**
2. **block-level 持久化存储能力**
3. **磁盘中断驱动的 I/O 模型**
4. **buffer cache + LRU 管理**
5. **bitmap 管理磁盘空间**
6. **面向磁盘/缓存的系统调用接口**

内核从“纯内存系统”迈出了关键一步。

---

## 三、实验整体逻辑关系理解

Lab-7 的完整逻辑路径为：

```
用户程序
  ↓ 系统调用
syscall / sysfunc
  ↓
文件系统 (fs.c)
  ↓
buffer 缓冲系统 (buffer.c)
  ↓
bitmap (bitmap.c)
  ↓
virtio 磁盘驱动
  ↓
磁盘中断 → trap_kernel → plic
```

其中：

* bitmap 决定 **资源是否可用**
* buffer 决定 **访问方式与性能**
* trap / 中断保证 **I/O 的异步性与安全性**

---

## 四、实验带来的思考

1. **陷阱（trap）是 OS 的中枢机制**

   * 系统调用依赖 trap
   * 磁盘 I/O 完成依赖 trap
   * 没有 trap，就没有用户态与内核态的安全协作

2. **I/O 必须是异步的**

   * 磁盘操作耗时
   * 通过 sleep / wakeup + 中断实现并发执行

3. **文件系统的本质不是“文件 API”**

   * 而是 bitmap + buffer + block 的协同管理

---

## 五、测试结果（基于实验要求）

### Test-1：读取 superblock

**测试目的**：验证磁盘、buffer、文件系统初始化是否成功

📸 测试截图位置：
`./pictures/test-1.png`

---

### Test-2：bitmap 申请与释放

**测试目的**：验证 data bitmap / inode bitmap 的正确性

📸 测试截图位置：
`./pictures/test-2.png`

---

### Test-3：buffer 读写与 LRU 管理

**测试目的**：

* 验证 block 读写正确性
* 验证 LRU 链表调整
* 验证 flush 释放物理内存

📸 测试截图位置：

* `./pictures/test-3(1).png`
* `./pictures/test-3(2).png`

---

## 总结

Lab-7 是从“内存型内核”迈向“持久化文件系统”的关键一步，为 Lab-8 的 inode 与层次化文件系统奠定了完整基础。

```

---

