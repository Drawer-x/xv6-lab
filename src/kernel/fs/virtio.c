#include "mod.h"
#include "../proc/method.h"   // proc_sleep / proc_wakeup
#include "../mem/method.h"    // vm_getpte 等

static __attribute__((aligned(PGSIZE))) disk_t disk;

/*===================== 内部：已持锁处理 used ring 的助手 =====================*/
static void virtio_service_used_locked(void)
{
    // 设备把已完成的条目写入 used->elems，并把 used->idx 递增。
    // 我们用 disk.used_idx 跟着消费；两者不相等就说明还有未处理的完成项。
    while (disk.used_idx != disk.used->idx) {
        // 对 ring 大小取模索引
        uint16 u = (uint16)(disk.used_idx % VIRTIO_NUM);

        int id = disk.used->elems[u].id;   // id = 我们在 avail 中提交的首描述符索引（idx[0]）
        if (disk.info[id].status != 0) {
            panic("virtio_disk_intr status");
        }

        // 唤醒等待这个 buffer 的睡眠者
        if (disk.info[id].b) {
            disk.info[id].b->disk = false;   // 标记 I/O 完成
            proc_wakeup(disk.info[id].b);
        }

        // 前进本地消费者下标
        disk.used_idx++;
    }

    // ACK 中断（把中断挂起位清掉），避免重复触发
    *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
}

/*============================== 设备初始化 ==============================*/
void virtio_disk_init()
{
    uint32 status = 0;

    spinlock_init(&disk.vdisk_lock, "virtio_disk");

    // 1) 基本识别：魔数/版本/设备/厂商
    if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(VIRTIO_MMIO_VERSION) != 1 ||
        *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
        *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
        panic("could not find virtio disk");
    }

    // 2) 驱动握手：ACK -> DRIVER
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(VIRTIO_MMIO_STATUS) = status;
    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 3) 协商特性：屏蔽掉不支持项
    uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

    // 4) 特性协商完成
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 5) legacy 接口：页大小
    *R(VIRTIO_MMIO_GUEST_PAGE_SIZE) = PGSIZE;

    // 6) 初始化队列0
    *R(VIRTIO_MMIO_QUEUE_SEL) = 0;
    uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0)         panic("virtio: no queue 0");
    if (max < VIRTIO_NUM) panic("virtio: queue too short");

    *R(VIRTIO_MMIO_QUEUE_NUM)   = VIRTIO_NUM;
    *R(VIRTIO_MMIO_QUEUE_ALIGN) = PGSIZE;  // 对齐要求

    // 7) 准备2页连续内存作为 desc/avail/used
    memset(disk.pages, 0, sizeof(disk.pages));
    *R(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk.pages) >> 12; // PFN

    // 8) 指针布局
    disk.desc  = (vring_desc_t *)disk.pages;
    disk.avail = (uint16 *)(((char *)disk.desc) + VIRTIO_NUM * sizeof(vring_desc_t));
    disk.used  = (used_area_t *)(disk.pages + PGSIZE);

    for (int i = 0; i < VIRTIO_NUM; i++)
        disk.free[i] = 1;

    // host/device 完成队列的读写下标
    disk.used_idx  = 0;
    disk.used->idx = 0;   // 注意：你的 used_area_t 必须有 idx 字段（若仍叫 id，请统一为 idx）

    // 9) 队列就绪（非常关键）
    *R(VIRTIO_MMIO_QUEUE_READY) = 1;

    // 10) 驱动就绪
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;
}

/*============================== 描述符分配 ==============================*/
static int alloc_desc()
{
    for (int i = 0; i < VIRTIO_NUM; i++) {
        if (disk.free[i]) {
            disk.free[i] = 0;
            return i;
        }
    }
    return -1;
}

static void free_desc(int i)
{
    if (i >= VIRTIO_NUM) panic("virtio_disk_intr 1");
    if (disk.free[i])    panic("virtio_disk_intr 2");
    disk.desc[i].addr = 0;
    disk.free[i] = 1;
    proc_wakeup(&disk.free[0]);
}

static void free_chain(int i)
{
    while (1) {
        free_desc(i);
        if (disk.desc[i].flags & VRING_DESC_F_NEXT)
            i = disk.desc[i].next;
        else
            break;
    }
}

static int alloc3_desc(int *idx)
{
    for (int i = 0; i < 3; i++) {
        idx[i] = alloc_desc();
        if (idx[i] < 0) {
            for (int j = 0; j < i; j++)
                free_desc(idx[j]);
            return -1;
        }
    }
    return 0;
}

/*============================== I/O 提交 ==============================*/
void virtio_disk_rw(buffer_t *b, bool write)
{
    uint64 sector = b->block_num * (BLOCK_SIZE / 512);

    spinlock_acquire(&disk.vdisk_lock);

    // 三个描述符：header / data / status
    int idx[3];
    while (1) {
        if (alloc3_desc(idx) == 0)
            break;
        proc_sleep(&disk.free[0], &disk.vdisk_lock);
    }

    struct virtio_blk_outhdr {
        uint32 type;
        uint32 reserved;
        uint64 sector;
    } buf0;

    buf0.type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    buf0.reserved = 0;
    buf0.sector = sector;

    // 将栈上的 buf0 转为物理地址（通过查 PTE）
    uint64 addr = ALIGN_DOWN((uint64)&buf0, PGSIZE);
    uint64 off  = ((uint64)&buf0) % PGSIZE;
    pte_t *pte  = vm_getpte(NULL, addr, false);

    disk.desc[idx[0]].addr  = (uint64)PTE_TO_PA(*pte) + off;
    disk.desc[idx[0]].len   = sizeof(buf0);
    disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
    disk.desc[idx[0]].next  = idx[1];

    disk.desc[idx[1]].addr  = (uint64)b->data;
    disk.desc[idx[1]].len   = BLOCK_SIZE;
    disk.desc[idx[1]].flags = write ? 0 : VRING_DESC_F_WRITE; // 写盘：设备读 data；读盘：设备写 data
    disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
    disk.desc[idx[1]].next  = idx[2];

    disk.info[idx[0]].status = 0;
    disk.desc[idx[2]].addr   = (uint64)&disk.info[idx[0]].status;
    disk.desc[idx[2]].len    = 1;
    disk.desc[idx[2]].flags  = VRING_DESC_F_WRITE; // 设备写入 status
    disk.desc[idx[2]].next   = 0;

    // 记录，供完成时唤醒
    b->disk = true;
    disk.info[idx[0]].b = b;

    // 提交给设备
    disk.avail[2 + (disk.avail[1] % VIRTIO_NUM)] = idx[0];
    __sync_synchronize();
    disk.avail[1] = (uint16)(disk.avail[1] + 1);

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // 通知队列 0

    // —— 轮询兜底：在已持锁情况下直接服务 used ring，避免丢中断卡死 —— 
    for (int spins = 0; spins < 100000 && b->disk; spins++) {
        uint32 isr = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
        if (isr) {
            virtio_service_used_locked();  // 注意：此处已持有 disk.vdisk_lock
            break;
        }
    }

    // 等待完成（正常由中断或上面的兜底唤醒）
    while (b->disk == true)
        proc_sleep(b, &disk.vdisk_lock);

    disk.info[idx[0]].b = 0;
    free_chain(idx[0]);

    spinlock_release(&disk.vdisk_lock);
}

/*============================== 中断处理 ==============================*/
void virtio_disk_intr()
{
    spinlock_acquire(&disk.vdisk_lock);
    virtio_service_used_locked();   // 已持锁处理完成队列
    spinlock_release(&disk.vdisk_lock);
}
