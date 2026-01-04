/*
 * device.c - 设备文件的实现
 * 
 * 实现六种设备文件:
 * /dev/stdin  - 标准输入(行缓冲), 可读
 * /dev/stdout - 标准输出, 可写
 * /dev/stderr - 标准错误输出(前置"ERROR"), 可写
 * /dev/zero   - 零文件(可读到任意多的零字节), 可读
 * /dev/null   - 黑洞文件(读到0字节,写入多少都可以), 可读可写
 * /dev/gpt0   - 小彩蛋(预设问答GPT), 可写
 */

#include "mod.h"

/* 设备读写函数声明 */
static uint32 stdin_read(uint32 len, uint64 dst, bool is_user_dst);
static uint32 stdout_write(uint32 len, uint64 src, bool is_user_src);
static uint32 stderr_write(uint32 len, uint64 src, bool is_user_src);
static uint32 zero_read(uint32 len, uint64 dst, bool is_user_dst);
static uint32 null_read(uint32 len, uint64 dst, bool is_user_dst);
static uint32 null_write(uint32 len, uint64 src, bool is_user_src);
static uint32 gpt0_write(uint32 len, uint64 src, bool is_user_src);

/* 设备表 */
static device_t device_table[N_DEVICE] = {
    { "/dev/stdin",  DEV_STDIN,  true,  false, stdin_read,  NULL },
    { "/dev/stdout", DEV_STDOUT, false, true,  NULL,        stdout_write },
    { "/dev/stderr", DEV_STDERR, false, true,  NULL,        stderr_write },
    { "/dev/zero",   DEV_ZERO,   true,  false, zero_read,   NULL },
    { "/dev/null",   DEV_NULL,   true,  true,  null_read,   null_write },
    { "/dev/gpt0",   DEV_GPT0,   false, true,  NULL,        gpt0_write },
};

/* 标准输入读取 - 从控制台读取(行缓冲) */
static uint32 stdin_read(uint32 len, uint64 dst, bool is_user_dst) {
    return cons_read(len, dst, is_user_dst);
}

/* 标准输出写入 - 写到控制台 */
static uint32 stdout_write(uint32 len, uint64 src, bool is_user_src) {
    return cons_write(len, src, is_user_src);
}

/* 标准错误写入 - 先输出"ERROR: "再写到控制台 */
static uint32 stderr_write(uint32 len, uint64 src, bool is_user_src) {
    printf("ERROR: ");
    return cons_write(len, src, is_user_src);
}

/* 零设备读取 - 读取任意多的零字节 */
static uint32 zero_read(uint32 len, uint64 dst, bool is_user_dst) {
    char zero = 0;
    for (uint32 i = 0; i < len; i++) {
        if (is_user_dst) {
            proc_t *p = myproc();
            if (p) {
                uvm_copyout(p->pgtbl, dst + i, (uint64)&zero, 1);
            }
        } else {
            *(char*)(dst + i) = zero;
        }
    }
    return len;
}

/* 空设备读取 - 总是返回0字节 */
static uint32 null_read(uint32 len, uint64 dst, bool is_user_dst) {
    (void)len;
    (void)dst;
    (void)is_user_dst;
    return 0;
}

/* 空设备写入 - 接受任意字节但丢弃 */
static uint32 null_write(uint32 len, uint64 src, bool is_user_src) {
    (void)src;
    (void)is_user_src;
    return len;
}

/* GPT设备写入 - 预设问答 */
static uint32 gpt0_write(uint32 len, uint64 src, bool is_user_src)
{
    char buf[128];
    uint32 read_len = len < 127 ? len : 127;

    if (is_user_src) {
        proc_t *p = myproc();
        if (p) {
            uvm_copyin(p->pgtbl, (uint64)buf, src, read_len);
        }
    } else {
        memmove(buf, (void*)src, read_len);
    }
    buf[read_len] = '\0';

    // === 最小改动：做一个小写副本用于匹配（兼容 Hello/GOOD JOB 等） ===
    char key[128];
    memmove(key, buf, read_len + 1);
    for (uint32 i = 0; key[i]; i++) {
        if (key[i] >= 'A' && key[i] <= 'Z')
            key[i] = key[i] - 'A' + 'a';
    }

    // === 按标准输出改问答 ===
    if (strncmp(key, "hello", 5) == 0) {
        printf("Hi, I am gpt0!\n");

    } else if (strncmp(key, "guess who i am", 14) == 0) {
        proc_t *p = myproc();
        if (p)
            printf("Your procid is %d and name is %s.\n", p->pid, p->name);
        else
            printf("Your procid is -1 and name is unknown.\n");

    } else if (strncmp(key, "how many free memory left", 25) == 0) {
        uint32 kfree = 0, ufree = 0;
        pmem_stat(&kfree, &ufree);
        printf("We have %d free pages in kernel space, %d free pages in user space!\n",
               kfree, ufree);

    } else if (strncmp(key, "good job", 8) == 0) {
        printf("Thanks for your kind words!\n");

    } else {
        printf("GPT0: I don't understand '%s'\n", buf);
    }

    return len;
}

/* 初始化设备表 - 确保各个设备文件在磁盘中存在 */
void device_init(void) {
    //printf("device_init: starting\n");
    
    // 首先确保/dev目录存在
    inode_t *dev_dir = path_to_inode("/dev");
    if (dev_dir == NULL) {
        //printf("device_init: creating /dev\n");
        dev_dir = path_create_inode("/dev", INODE_TYPE_DIR, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
        if (dev_dir != NULL) {
            //printf("device_init: /dev created\n");
            inode_put(dev_dir);
        } else {
            //printf("device_init: failed to create /dev!\n");
        }
    } else {
        //printf("device_init: /dev already exists\n");
        inode_put(dev_dir);
    }
    
    // 为每个设备创建设备文件
    for (int i = 0; i < N_DEVICE; i++) {
        if (device_table[i].name[0] == '\0') continue;  // 跳过空条目
        
        inode_t *ip = path_to_inode(device_table[i].name);
        if (ip == NULL) {
            //printf("device_init: creating %s\n", device_table[i].name);
            ip = path_create_inode(device_table[i].name, INODE_TYPE_DIVICE, 
                                   device_table[i].major, INODE_MINOR_DEFAULT);
            if (ip == NULL) {
                //printf("device_init: failed to create %s!\n", device_table[i].name);
                continue;
            }
        }
        inode_put(ip);
    }
    //printf("device_init: done\n");
}

/* 检查设备文件是否存在及打开权限的合法性 */
bool device_open_check(uint16 major, uint32 open_mode) {
    // 查找设备
    for (int i = 0; i < N_DEVICE; i++) {
        if (device_table[i].major == major) {
            // 使用新的标志位检查
            bool need_read = (open_mode & O_RDONLY) != 0;
            bool need_write = (open_mode & O_WRONLY) != 0;
            
            if (need_read && !device_table[i].readable) {
                return false;
            }
            if (need_write && !device_table[i].writable) {
                return false;
            }
            return true;
        }
    }
    return false;
}

/* 设备读取接口 */
uint32 device_read_data(uint16 major, uint32 len, uint64 dst, bool is_user_dst) {
    for (int i = 0; i < N_DEVICE; i++) {
        if (device_table[i].major == major) {
            if (device_table[i].read != NULL) {
                return device_table[i].read(len, dst, is_user_dst);
            }
            return 0;
        }
    }
    return 0;
}

/* 设备写入接口 */
uint32 device_write_data(uint16 major, uint32 len, uint64 src, bool is_user_src) {
    for (int i = 0; i < N_DEVICE; i++) {
        if (device_table[i].major == major) {
            if (device_table[i].write != NULL) {
                return device_table[i].write(len, src, is_user_src);
            }
            return 0;
        }
    }
    return 0;
}

