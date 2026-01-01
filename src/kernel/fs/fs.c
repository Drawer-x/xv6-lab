/*
 * fs.c - 文件系统核心实现
 * 
 * 实现文件的抽象和操作
 */

#include "mod.h"

super_block_t sb; /* 超级块 */

/* 文件资源池和保护锁 */
static file_t file_table[N_FILE];
static spinlock_t lk_file_table;

/* 初始化文件表和锁 */
void file_init(void) {
    spinlock_init(&lk_file_table, "file_table");
    for (int i = 0; i < N_FILE; i++) {
        file_table[i].ref = 0;
        file_table[i].ip = NULL;
        file_table[i].readable = false;
        file_table[i].writable = false;
        file_table[i].offset = 0;
    }
}

/* 获取空闲file */
file_t* file_alloc(void) {
    spinlock_acquire(&lk_file_table);
    for (int i = 0; i < N_FILE; i++) {
        if (file_table[i].ref == 0) {
            file_table[i].ref = 1;
            spinlock_release(&lk_file_table);
            return &file_table[i];
        }
    }
    spinlock_release(&lk_file_table);
    return NULL;
}

/* 打开文件
 * open_mode: O_CREATE, O_RDONLY, O_WRONLY
 */
file_t* file_open(char *path, uint32 open_mode) {
    inode_t *ip;
    
    ip = path_to_inode(path);
    
    if (ip == NULL) {
        printf("file_open: path_to_inode(%s) failed, mode=0x%x, O_CREATE=0x%x\n", path, open_mode, O_CREATE);
        if (open_mode & O_CREATE) {
            ip = path_create_inode(path, INODE_TYPE_DATA, INODE_MAJOR_DEFAULT, INODE_MINOR_DEFAULT);
            if (ip == NULL) {
                printf("file_open: path_create_inode(%s) failed\n", path);
                return NULL;
            }
            printf("file_open: created %s, inode=%d\n", path, ip->inode_num);
        } else {
            return NULL;
        }
    }
    
    printf("file_open: about to lock inode for %s\n", path);
    
    inode_lock(ip);
    
    if (ip->disk_info.type == INODE_TYPE_DIVICE) {
        if (!device_open_check(ip->disk_info.major, open_mode)) {
            printf("file_open: device_open_check failed, major=%d, mode=%d\n", 
                   ip->disk_info.major, open_mode);
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }
    }
    
    // 分配file结构体
    file_t *f = file_alloc();
    if (f == NULL) {
        inode_unlock(ip);
        inode_put(ip);
        return NULL;
    }
    
    // 设置文件属性
    f->ip = ip;
    f->offset = 0;
    
    // 设置读写权限 (使用新的标志位)
    f->readable = (open_mode & O_RDONLY) != 0;
    f->writable = (open_mode & O_WRONLY) != 0;
    
    // 如果需要截断文件
    if ((open_mode & O_TRUNC) && ip->disk_info.type == INODE_TYPE_DATA) {
        // 简单处理：将size设为0
        ip->disk_info.size = 0;
        inode_rw(ip, true);
    }
    
    inode_unlock(ip);
    
    return f;
}

/* 关闭文件 */
void file_close(file_t *file) {
    if (file == NULL) return;
    
    spinlock_acquire(&lk_file_table);
    
    if (file->ref < 1) {
        spinlock_release(&lk_file_table);
        panic("file_close: ref < 1");
    }
    
    file->ref--;
    
    if (file->ref == 0) {
        // 最后一个引用被释放
        inode_t *ip = file->ip;
        file->ip = NULL;
        spinlock_release(&lk_file_table);
        
        if (ip != NULL) {
            inode_put(ip);
        }
    } else {
        spinlock_release(&lk_file_table);
    }
}

/* 读取文件 
 * 返回实际读取的字节数
 */
uint32 file_read(file_t* file, uint32 len, uint64 dst, bool is_user_dst) {
    if (file == NULL || !file->readable) {
        return 0;
    }
    
    inode_t *ip = file->ip;
    if (ip == NULL) return 0;
    
    uint32 ret = 0;
    
    // 设备文件不需要锁（会阻塞），直接调用设备函数
    if (ip->disk_info.type == INODE_TYPE_DIVICE) {
        return device_read_data(ip->disk_info.major, len, dst, is_user_dst);
    }
    
    inode_lock(ip);
    
    switch (ip->disk_info.type) {
    case INODE_TYPE_DATA:
        // 数据文件: 从inode读取数据
        ret = inode_read_data(ip, file->offset, len, (void*)dst, is_user_dst);
        file->offset += ret;
        break;
        
    case INODE_TYPE_DIR:
        // 目录文件: 读取目录项
        ret = dentry_transmit(ip, dst, len, is_user_dst);
        break;
        
    default:
        ret = 0;
    }
    
    inode_unlock(ip);
    
    return ret;
}

/* 写入文件
 * 返回实际写入的字节数
 */
uint32 file_write(file_t* file, uint32 len, uint64 src, bool is_user_src) {
    if (file == NULL || !file->writable) {
        return 0;
    }
    
    inode_t *ip = file->ip;
    if (ip == NULL) return 0;
    
    uint32 ret = 0;
    
    // 设备文件不需要锁，直接调用设备函数
    if (ip->disk_info.type == INODE_TYPE_DIVICE) {
        return device_write_data(ip->disk_info.major, len, src, is_user_src);
    }
    
    inode_lock(ip);
    
    switch (ip->disk_info.type) {
    case INODE_TYPE_DATA:
        // 数据文件: 向inode写入数据
        ret = inode_write_data(ip, file->offset, len, (void*)src, is_user_src);
        file->offset += ret;
        inode_rw(ip, true);
        break;
        
    case INODE_TYPE_DIR:
        // 目录文件: 不允许直接写入
        ret = 0;
        break;
        
    default:
        ret = 0;
    }
    
    inode_unlock(ip);
    
    return ret;
}

/* 移动读写指针
 * lseek_flag: SEEK_SET, SEEK_CUR, SEEK_END
 * 返回新的偏移量, 失败返回-1
 */
uint32 file_lseek(file_t *file, uint32 lseek_offset, uint32 lseek_flag) {
    if (file == NULL) {
        return (uint32)-1;
    }
    
    inode_t *ip = file->ip;
    if (ip == NULL) return (uint32)-1;
    
    inode_lock(ip);
    
    uint32 new_offset;
    
    switch (lseek_flag) {
    case LSEEK_SET:
        new_offset = lseek_offset;
        break;
    case LSEEK_ADD:
        new_offset = file->offset + lseek_offset;
        break;
    case LSEEK_SUB:
        new_offset = file->offset - lseek_offset;
        break;
    default:
        inode_unlock(ip);
        return (uint32)-1;
    }
    
    file->offset = new_offset;
    
    inode_unlock(ip);
    
    return new_offset;
}

/* 复制文件使用权 (file->ref++) */
file_t* file_dup(file_t* file) {
    if (file == NULL) return NULL;
    
    spinlock_acquire(&lk_file_table);
    
    if (file->ref < 1) {
        spinlock_release(&lk_file_table);
        panic("file_dup: ref < 1");
    }
    
    file->ref++;
    
    spinlock_release(&lk_file_table);
    
    return file;
}

/* 获取文件状态 */
uint32 file_get_stat(file_t* file, uint64 user_dst) {
    if (file == NULL || file->ip == NULL) {
        return (uint32)-1;
    }
    
    inode_t *ip = file->ip;
    file_stat_t st;
    
    inode_lock(ip);
    
    st.type = ip->disk_info.type;
    st.nlink = ip->disk_info.nlink;
    st.size = ip->disk_info.size;
    st.inode_num = ip->inode_num;
    st.offset = file->offset;
    
    inode_unlock(ip);
    
    // 复制到用户空间
    proc_t *p = myproc();
    if (p) {
        uvm_copyout(p->pgtbl, user_dst, (uint64)&st, sizeof(file_stat_t));
    }
    
    return 0;
}

/* 文件系统初始化 */
void fs_init(void)
{
    /* 1. 初始化 buffer cache（最先） */
    buffer_init();

    /* 2. 读取 super block（固定在 0 号块） */
    buffer_t *buf = buffer_get(0);
    memmove(&sb, buf->data, sizeof(super_block_t));
    buffer_put(buf);
    
    printf("fs_init: sb.magic=%x, sb.inode_firstblock=%d\n", sb.magic_num, sb.inode_firstblock);

    /* 3. 初始化 inode cache */
    inode_init();
    
    /* 4. 初始化 file table */
    file_init();
    
    /* 5. 初始化设备表 */
    device_init();
    
    printf("fs_init: file system initialized\n");
}
