// in initcode.c
#include "sys.h"

#define PGSIZE 4096

int main()
{
    long long heap_top = 0;
    
    heap_top = syscall(SYS_brk, 0);
    heap_top = syscall(SYS_brk, heap_top + PGSIZE * 9);
    heap_top = syscall(SYS_brk, heap_top);
    heap_top = syscall(SYS_brk, heap_top - PGSIZE * 5);

    while(1);
    return 0;
}