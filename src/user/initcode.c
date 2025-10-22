#include "sys.h"

int main()
{
    syscall(SYS_helloworld);  // 第一次系统调用
    syscall(SYS_helloworld);  // 第二次系统调用
    while (1)
        ;
    return 0;
}
