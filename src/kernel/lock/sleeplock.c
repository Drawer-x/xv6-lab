#include "mod.h"
#include "../proc/mod.h" // 依赖进程管理函数（proc_sleep、proc_wakeup、mycpu）

// 睡眠锁初始化
void sleeplock_init(sleeplock_t *lk, char *name)
{
    // 初始化内部保护自旋锁
    spinlock_init(&lk->lock, "sleeplock_inner");
    lk->locked = 0;       // 初始状态：未上锁
    lk->name = name;      // 锁名称（用于调试）
    lk->pid = -1;         // 初始状态：无持有进程
}

// 检查当前进程是否持有睡眠锁
bool sleeplock_holding(sleeplock_t *lk)
{
    bool ret;
    // 获取内部自旋锁，确保原子检查
    spinlock_acquire(&lk->lock);
    // 对比当前进程PID与锁的持有PID
    ret = (lk->pid == mycpu()->proc->pid);
    spinlock_release(&lk->lock);
    return ret;
}

// 当前进程尝试获取睡眠锁, 失败进入睡眠状态
void sleeplock_acquire(sleeplock_t *lk)
{
    // 1. 获取内部自旋锁，保护后续操作原子性
    spinlock_acquire(&lk->lock);
    
    // 2. 循环等待：锁被持有则睡眠，直到锁可用
    while (lk->locked) {
        // 睡眠资源设为当前睡眠锁，释放内部自旋锁后睡眠
        // 注：proc_sleep会在睡眠前释放传入的锁，唤醒后重新获取
        proc_sleep(lk, &lk->lock);
        // 被唤醒后，已重新持有内部自旋锁，再次检查锁状态
    }
    
    // 3. 成功获取锁：标记为持有，记录当前进程PID
    lk->locked = 1;
    lk->pid = mycpu()->proc->pid;
    
    // 4. 释放内部自旋锁
    spinlock_release(&lk->lock);
}

// 释放睡眠锁, 唤醒其他等待睡眠锁的进程
void sleeplock_release(sleeplock_t *lk)
{
    // 1. 获取内部自旋锁，保护后续操作原子性
    spinlock_acquire(&lk->lock);
    
    // 2. 检查：只有持有锁的进程才能释放
    assert(lk->locked == 1 && lk->pid == mycpu()->proc->pid, 
           "sleeplock_release: not holder");
    
    // 3. 标记锁为未持有，清空PID
    lk->locked = 0;
    lk->pid = -1;
    
    // 4. 唤醒所有等待该睡眠锁的进程（睡眠资源为lk）
    proc_wakeup(lk);
    
    // 5. 释放内部自旋锁
    spinlock_release(&lk->lock);
}