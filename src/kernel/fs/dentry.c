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
	根据文件路径(/A/B/C)查找对应inode(inode_B or inode_C)
	如果find_parent_inode == true, 返回父节点inode, name为下一级子节点的名字
	如果find_parent_inode == false, 返回子节点inode, name无意义
	如果失败返回NULL
*/
static inode_t* __path_to_inode(char *path, char *name, bool find_parent_inode)
{
    inode_t *ip = inode_get(ROOT_INODE);
    char elem[MAXLEN_FILENAME];
    char *p = path;

    while ((p = get_element(p, elem)) != NULL) {
        inode_lock(ip);

        if (ip->disk_info.type != INODE_TYPE_DIR) {
            inode_unlock(ip);
            inode_put(ip);
            return NULL;
        }

        if (find_parent_inode && *p == 0) {
            memmove(name, elem, MAXLEN_FILENAME);
            inode_unlock(ip);
            return ip;
        }

        uint32 ino = dentry_search(ip, elem);
        inode_unlock(ip);

        if (ino == INVALID_INODE_NUM) {
            inode_put(ip);
            return NULL;
        }

        inode_t *next = inode_get(ino);
        inode_put(ip);
        ip = next;
    }

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
