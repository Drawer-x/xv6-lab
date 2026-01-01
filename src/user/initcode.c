/*
 * initcode.c - 第一个用户进程
 * 自动生成 - test_2
 */
#include "sys.h"

int main()
{
    char *msg = "run ./test_2\n";
    syscall(SYS_write, 1, 13, msg);
    
    int pid = syscall(SYS_fork);
    
    if (pid == 0) {
        char *argv[] = {"test_2", 0};
        syscall(SYS_exec, "/test_2", argv);
        syscall(SYS_exit, -1);
    } else if (pid > 0) {
        int status;
        syscall(SYS_wait, &status);
    }
    
    while(1);
}
