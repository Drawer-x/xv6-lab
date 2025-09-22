#include "arch/mod.h"
#include "lib/mod.h"

int main()
{
    print_init(); // 初始化打印系统
    
    printf("cpu %d is booting!\n", r_tp());


    return 0;
}