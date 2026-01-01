#include "mod.h"

/*
	出于简化目的的假设:
	如果inode_disk.type == INODE_TYPE_DIR
	那么inode_disk.size <= BLOCKSIZE (只有inode_disk.index[0]有效)
	也就是说, 单个目录最多包含BLOCKSIZE / sizeof(dentry)个目录项

	另外, INODE_TYPE_DATA要求数据之间没有空隙
	但是对于INODE_TYPE_DIR来说是无法做到的(目录项的删除很常见)
	因此, ip->size代表block中已经使用的空间大小
*/


/*----------------dentry的查找、增加、删除操作-----------------*/

/*
	在目录ip中查找是否存在名字为name的目录项
	如果找到了返回目录项中存储的inode_num
	如果没找到返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_search(inode_t *ip, char *name)
{
    assert(sleeplock_holding(&ip->slk), "dentry_search: slk!");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_search: not dir!");

    if (ip->disk_info.index[0] == 0)
        return INVALID_INODE_NUM;

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de;

    for (de = (dentry_t *)buf->data;
         de < (dentry_t *)(buf->data + BLOCK_SIZE);
         de++)
    {
        if (de->name[0] == 0)
            continue;
        if (strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
            uint32 ino = de->inode_num;
            buffer_put(buf);
            return ino;
        }
    }

    buffer_put(buf);
    return INVALID_INODE_NUM;
}

/*
	在目录ip下删除名称为name的dentry, 返回它的inode_num
	如果匹配失败或者遇到非法情况返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_create(inode_t *ip, uint32 inode_num, char *name)
{
    assert(sleeplock_holding(&ip->slk), "dentry_create: slk!");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_create: not dir!");

    /* 目录只使用 index[0] */
    if (ip->disk_info.index[0] == 0) {
        uint32 b = bitmap_alloc_block();
        if (b == 0)
            return (uint32)-1;
        ip->disk_info.index[0] = b;
    }

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de;
    dentry_t *empty = NULL;

    for (de = (dentry_t *)buf->data;
         de < (dentry_t *)(buf->data + BLOCK_SIZE);
         de++)
    {
        if (de->name[0] == 0) {
            if (empty == NULL)
                empty = de;
            continue;
        }
        if (strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
            buffer_put(buf);
            return (uint32)-1;   // 重名
        }
    }

    if (empty == NULL) {
        buffer_put(buf);
        return (uint32)-1;       // 没空间
    }

    /* 写入新 dentry */
    memset(empty, 0, sizeof(dentry_t));
    memmove(empty->name, name, MAXLEN_FILENAME);
    empty->name[MAXLEN_FILENAME - 1] = '\0';
    empty->inode_num = inode_num;

    buffer_write(buf);
    buffer_put(buf);

    uint32 offset = (uint32)((uint8 *)empty - buf->data);
    if (offset + sizeof(dentry_t) > ip->disk_info.size)
        ip->disk_info.size = offset + sizeof(dentry_t);

    return offset;
}

uint32 dentry_delete(inode_t *ip, char *name)
{
    assert(sleeplock_holding(&ip->slk), "dentry_delete: slk!");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_delete: not dir!");

    if (ip->disk_info.index[0] == 0)
        return INVALID_INODE_NUM;

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de;

    for (de = (dentry_t *)buf->data;
         de < (dentry_t *)(buf->data + BLOCK_SIZE);
         de++)
    {
        if (de->name[0] == 0)
            continue;
        if (strncmp(de->name, name, MAXLEN_FILENAME) == 0) {
            uint32 ino = de->inode_num;
            memset(de, 0, sizeof(dentry_t));
            buffer_write(buf);
            buffer_put(buf);
            return ino;
        }
    }

    buffer_put(buf);
    return INVALID_INODE_NUM;
}

/* 输出目录中所有有效目录项的信息 (for debug) */
void dentry_print(inode_t *ip)
{
	assert(sleeplock_holding(&ip->slk), "dentry_print: slk!");
	assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_print: not dir!");

	dentry_t *de;
	buffer_t *buf;

	if (ip->disk_info.index[0] == 0)
		panic("dentry_print: invalid index[0]!");
	
	printf("inode_num = %d, dentries:\n", ip->inode_num);

	buf = buffer_get(ip->disk_info.index[0]);
	for (de = (dentry_t*)(buf->data); de < (dentry_t*)(buf->data + BLOCK_SIZE); de++)
	{
		if (de->name[0] != 0) {
			printf("dentry: offset = %d, inode_num = %d, name = %s\n",
				(uint32)((uint8*)de - buf->data), de->inode_num, de->name);
		}
	}
	buffer_put(buf);

	printf("\n");
}

/*------------------从文件名到文件路径-----------------*/

/*
	Examples:
	get_element("a/bb/c", name) = "bb/c" + name = "a"
	get_element("///aa//bb", name) = "bb" + name = "aa"
	get_element("aaa", name) = "" + name = "aaa"
	get_element("", name) = NULL + name = ""
	get_element("//", name) = NULL + name = ""
*/
static char* get_element(char *path, char *name)
{
	/* 跳过前置的'/' */
    while (*path == '/')
		path++;

	/* 如果遇到末尾了则返回 */
    if (*path == 0) {
		name[0] = 0;
		return NULL;
	}

	/* 记录起点位置 */
    char *start = path;
    
	/* 推进path直到遇到'/'或者到达末尾 */
	while (*path != '/' && *path != 0)
        path++;

	/* 提取到的name的长度 */
    int len = path - start;
	len = MIN(len, MAXLEN_FILENAME-1);
	
	/* 设置name */
	memmove(name, start, len);
	name[len] = 0;

	/* 跳过后置的'/' */
    while (*path == '/') path++;

    return path;
}
/*
	根据文件路径查找对应inode
	支持绝对路径(/A/B/C)和相对路径(A/B/C, ./A, ../A)
	如果find_parent_inode == true, 返回父节点inode, name为下一级子节点的名字
	如果find_parent_inode == false, 返回子节点inode, name无意义
	如果失败返回NULL
*/
static inode_t* __path_to_inode(char *path, char *name, bool find_parent_inode)
{
    inode_t *ip;
    char elem[MAXLEN_FILENAME];
    char *p = path;
    
    //printf("__path_to_inode: path='%s', find_parent=%d\n", path, find_parent_inode);
    
    // 判断是绝对路径还是相对路径
    if (*path == '/') {
        ip = inode_get(ROOT_INODE);
        //printf("__path_to_inode: got root inode, ip=%p\n", ip);
    } else {
        proc_t *proc = myproc();
        if (proc != NULL && proc->cwd != NULL) {
            ip = inode_dup(proc->cwd);
        } else {
            ip = inode_get(ROOT_INODE);
        }
    }

    if (ip == NULL) {
        //printf("__path_to_inode: ip is NULL\n");
        return NULL;
    }

    while ((p = get_element(p, elem)) != NULL) {
        //printf("__path_to_inode: elem='%s', *p=%d\n", elem, *p);
        inode_lock(ip);

        //printf("__path_to_inode: ip type=%d\n", ip->disk_info.type);
        if (ip->disk_info.type != INODE_TYPE_DIR) {
            //printf("__path_to_inode: %s is not a dir (type=%d)\n", elem, ip->disk_info.type);
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }

        if (find_parent_inode && *p == 0) {
            //printf("__path_to_inode: returning parent, elem='%s'\n", elem);
            memmove(name, elem, MAXLEN_FILENAME);
            inode_unlock(ip);
            return ip;
        }

        uint32 ino = dentry_search(ip, elem);
        inode_unlock(ip);

        if (ino == INVALID_INODE_NUM) {
            //printf("__path_to_inode: elem '%s' not found in dir\n", elem);
            inode_put(ip);
            return NULL;
        }

        inode_t *next = inode_get(ino);
        inode_put(ip);
        ip = next;
    }

    //printf("__path_to_inode: loop ended, find_parent=%d\n", find_parent_inode);
    if (find_parent_inode) {
        inode_put(ip);
        return NULL;
    }

    return ip;
}


/*
	基于path寻找inode
	失败返回NULL
*/
inode_t* path_to_inode(char *path)
{
	char name[MAXLEN_FILENAME];
	return __path_to_inode(path, name, false);
}

/* 
	基于path寻找inode->parent, 将inode->name放入name
	失败返回NULL, 同时name无效
*/
inode_t* path_to_parent_inode(char *path, char *name)
{
	return __path_to_inode(path, name, true);
}

/*
	基于inode_num在目录ip中搜索对应的name
	如果找到了返回目录项偏移量
	如果没找到返回INVALID_INODE_NUM
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_search_2(inode_t *ip, uint32 inode_num, char *name)
{
    assert(sleeplock_holding(&ip->slk), "dentry_search_2: slk!");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_search_2: not dir!");

    if (ip->disk_info.index[0] == 0)
        return INVALID_INODE_NUM;

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de;

    for (de = (dentry_t *)buf->data;
         de < (dentry_t *)(buf->data + BLOCK_SIZE);
         de++)
    {
        if (de->name[0] == 0)
            continue;
        if (de->inode_num == inode_num) {
            memmove(name, de->name, MAXLEN_FILENAME);
            uint32 offset = (uint32)((uint8 *)de - buf->data);
            buffer_put(buf);
            return offset;
        }
    }

    buffer_put(buf);
    return INVALID_INODE_NUM;
}

/*
	传输目录中所有有效目录项到dst
	返回传输的字节数
	注意: 调用者需要持有ip->slk
*/
uint32 dentry_transmit(inode_t *ip, uint64 dst, uint32 len, bool is_user_dst)
{
    assert(sleeplock_holding(&ip->slk), "dentry_transmit: slk!");
    assert(ip->disk_info.type == INODE_TYPE_DIR, "dentry_transmit: not dir!");

    if (ip->disk_info.index[0] == 0)
        return 0;

    buffer_t *buf = buffer_get(ip->disk_info.index[0]);
    dentry_t *de;
    uint32 copied = 0;

    for (de = (dentry_t *)buf->data;
         de < (dentry_t *)(buf->data + BLOCK_SIZE) && copied + sizeof(dentry_t) <= len;
         de++)
    {
        if (de->name[0] == 0)
            continue;
        
        // 复制这个目录项
        if (is_user_dst) {
            proc_t *p = myproc();
            if (p) {
                uvm_copyout(p->pgtbl, dst + copied, (uint64)de, sizeof(dentry_t));
            }
        } else {
            memmove((void*)(dst + copied), de, sizeof(dentry_t));
        }
        copied += sizeof(dentry_t);
    }

    buffer_put(buf);
    return copied;
}

/*
	获取某个inode(目录类型)的绝对路径
	逆向填充方法: 从后往前填充缓冲区
	返回偏移量(path + offset是绝对路径字符串的起点)
	注意: 输入的ip未上锁, 内部会进行加锁解锁
*/
uint32 inode_to_path(inode_t *ip, char *path, uint32 len)
{
    if (len == 0) return 0;
    
    // 从缓冲区末尾开始填充
    uint32 offset = len - 1;
    path[offset] = '\0';
    
    // 处理根目录的特殊情况
    if (ip->inode_num == ROOT_INODE) {
        if (offset > 0) {
            offset--;
            path[offset] = '/';
        }
        return offset;
    }
    
    inode_t *current = inode_dup(ip);
    char name[MAXLEN_FILENAME];
    
    while (current->inode_num != ROOT_INODE) {
        inode_lock(current);
        
        // 获取父目录的inode_num (通过".."目录项)
        uint32 parent_ino = dentry_search(current, "..");
        if (parent_ino == INVALID_INODE_NUM) {
            inode_unlock(current);
            inode_put(current);
            return offset;
        }
        
        inode_unlock(current);
        
        // 获取父目录inode
        inode_t *parent = inode_get(parent_ino);
        inode_lock(parent);
        
        // 在父目录中查找当前inode的名字
        if (dentry_search_2(parent, current->inode_num, name) == INVALID_INODE_NUM) {
            inode_unlock(parent);
            inode_put(parent);
            inode_put(current);
            return offset;
        }
        
        inode_unlock(parent);
        
        // 计算名字长度
        int name_len = strlen(name);
        
        // 检查是否有足够空间
        if (offset < (uint32)(name_len + 1)) {
            inode_put(parent);
            inode_put(current);
            return offset;
        }
        
        // 从后往前填充名字
        offset -= name_len;
        memmove(path + offset, name, name_len);
        
        // 添加'/'
        offset--;
        path[offset] = '/';
        
        // 移动到父目录
        inode_put(current);
        current = parent;
    }
    
    inode_put(current);
    
    // 如果没有'/'开头,添加一个
    if (offset >= len || path[offset] != '/') {
        if (offset > 0) {
            offset--;
            path[offset] = '/';
        }
    }
    
    return offset;
}

/*
	基于目标路径创建新的inode
	包括inode的申请和目录项的创建等
	失败返回NULL
*/
inode_t* path_create_inode(char *path, uint16 type, uint16 major, uint16 minor)
{
    char name[MAXLEN_FILENAME];
    inode_t *parent;
    
    parent = path_to_parent_inode(path, name);
    if (parent == NULL) {
        //printf("path_create_inode: parent not found for %s\n", path);
        return NULL;
    }
    
    inode_lock(parent);
    
    uint32 exist_ino = dentry_search(parent, name);
    if (exist_ino != INVALID_INODE_NUM) {
        //printf("path_create_inode: %s already exists (ino=%d)\n", name, exist_ino);
        inode_unlock(parent);
        inode_put(parent);
        return NULL;
    }
    
    inode_t *ip = inode_create(type, major, minor);
    if (ip == NULL) {
        //printf("path_create_inode: inode_create failed for %s\n", name);
        inode_unlock(parent);
        inode_put(parent);
        return NULL;
    }
    //printf("path_create_inode: created inode %d, type=%d (expected %d)\n", 
    
    uint32 result = dentry_create(parent, ip->inode_num, name);
    if (result == (uint32)-1) {
        //printf("path_create_inode: dentry_create failed for %s\n", name);
        inode_lock(ip);
        ip->disk_info.nlink = 0;
        inode_unlock(ip);
        inode_put(ip);
        inode_unlock(parent);
        inode_put(parent);
        return NULL;
    }
    
    // 更新父目录
    inode_rw(parent, true);
    inode_unlock(parent);
    inode_put(parent);
    
    // 如果创建的是目录,需要添加.和..目录项
    if (type == INODE_TYPE_DIR) {
        inode_lock(ip);
        dentry_create(ip, ip->inode_num, ".");
        dentry_create(ip, parent->inode_num, "..");
        inode_rw(ip, true);
        inode_unlock(ip);
    }
    
    return ip;
}

/*
	建立硬链接: old_path的inode增加一个新的路径new_path
	成功返回0, 失败返回-1
*/
uint32 path_link(char *old_path, char *new_path)
{
    char name[MAXLEN_FILENAME];
    
    // 获取old_path对应的inode
    inode_t *ip = path_to_inode(old_path);
    if (ip == NULL) {
        return (uint32)-1;
    }
    
    inode_lock(ip);
    
    // 不能对目录建立硬链接
    if (ip->disk_info.type == INODE_TYPE_DIR) {
        inode_unlock(ip);
        inode_put(ip);
        return (uint32)-1;
    }
    
    // 增加链接计数
    ip->disk_info.nlink++;
    inode_rw(ip, true);
    inode_unlock(ip);
    
    // 获取new_path的父目录
    inode_t *parent = path_to_parent_inode(new_path, name);
    if (parent == NULL) {
        inode_lock(ip);
        ip->disk_info.nlink--;
        inode_rw(ip, true);
        inode_unlock(ip);
        inode_put(ip);
        return (uint32)-1;
    }
    
    inode_lock(parent);
    
    // 在父目录中创建目录项
    uint32 result = dentry_create(parent, ip->inode_num, name);
    if (result == (uint32)-1) {
        inode_unlock(parent);
        inode_put(parent);
        inode_lock(ip);
        ip->disk_info.nlink--;
        inode_rw(ip, true);
        inode_unlock(ip);
        inode_put(ip);
        return (uint32)-1;
    }
    
    inode_rw(parent, true);
    inode_unlock(parent);
    inode_put(parent);
    inode_put(ip);
    
    return 0;
}

/*
	解除硬链接
	1. inode.nlink--
	2. 删除一条dentry
	当nlink减到0时, inode_put会触发资源释放
	成功返回0, 失败返回-1
*/
uint32 path_unlink(char *path)
{
    char name[MAXLEN_FILENAME];
    
    // 获取父目录
    inode_t *parent = path_to_parent_inode(path, name);
    if (parent == NULL) {
        return (uint32)-1;
    }
    
    inode_lock(parent);
    
    // 在父目录中查找目录项
    uint32 ino = dentry_search(parent, name);
    if (ino == INVALID_INODE_NUM) {
        inode_unlock(parent);
        inode_put(parent);
        return (uint32)-1;
    }
    
    // 获取目标inode
    inode_t *ip = inode_get(ino);
    inode_lock(ip);
    
    // 不能unlink目录 (除非目录为空,这里简化处理不允许unlink目录)
    if (ip->disk_info.type == INODE_TYPE_DIR) {
        inode_unlock(ip);
        inode_put(ip);
        inode_unlock(parent);
        inode_put(parent);
        return (uint32)-1;
    }
    
    // 删除目录项
    dentry_delete(parent, name);
    inode_rw(parent, true);
    inode_unlock(parent);
    inode_put(parent);
    
    // 减少链接计数
    ip->disk_info.nlink--;
    inode_rw(ip, true);
    inode_unlock(ip);
    
    // inode_put会检查nlink==0并触发资源释放
    inode_put(ip);
    
    return 0;
}