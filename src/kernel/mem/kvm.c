#include "mod.h"

// 内核页表
pgtbl_t kernel_pgtbl;
extern char trampoline[];
// 根据pagetable,找到va对应的pte
// 若设置alloc=true 则在PTE无效时尝试申请一个物理页
// 成功返回PTE, 失败返回NULL
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if (va >= VA_MAX)
        return NULL;

    pte_t *pte;
    uint64 vpn[3];
    
    // 提取三级VPN
    vpn[2] = VA_TO_VPN(va, 2);
    vpn[1] = VA_TO_VPN(va, 1);
    vpn[0] = VA_TO_VPN(va, 0);

    // 遍历三级页表
    for (int level = 2; level > 0; level--) {
        pte = &pgtbl[vpn[level]];
        if (!(*pte & PTE_V)) {
            if (!alloc)
                return NULL;
            
            // 分配新物理页作为下一级页表
            uint64 pa = (uint64)pmem_alloc(true);
            if (!pa)
                return NULL;
            
            // 初始化页表物理页
            memset((void*)pa, 0, PGSIZE);
            
            // 设置页表项（仅V位有效，无读写执行权限）
            *pte = PA_TO_PTE(pa) | PTE_V;
        }
        
        // 验证当前页表项是页表指针
        if (!PTE_CHECK(*pte))
            return NULL;
        
        // 进入下一级页表
        pgtbl = (pgtbl_t)PTE_TO_PA(*pte);
    }

    // 返回最低级页表项
    return &pgtbl[vpn[0]];
}

// 在pgtbl中建立 [va, va + len) -> [pa, pa + len) 的映射
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    // 验证参数合法性
    assert((va % PGSIZE) == 0, "vm_mappages: va not aligned");
    assert((pa % PGSIZE) == 0, "vm_mappages: pa not aligned");
    assert(len > 0 && (len % PGSIZE) == 0, "vm_mappages: invalid len");
    assert(va + len <= VA_MAX, "vm_mappages: va out of range");

    uint64 curr_va = va;
    uint64 curr_pa = pa;
    
    while (curr_va < va + len) {
        pte_t *pte = vm_getpte(pgtbl, curr_va, true);
        assert(pte != NULL, "vm_mappages: getpte failed");

        // 如果已有映射，先解除
        if (*pte & PTE_V) {
            vm_unmappages(pgtbl, curr_va, PGSIZE, false);
        }

        // 设置新映射（物理地址+权限标志）
        *pte = PA_TO_PTE(curr_pa) | perm | PTE_V;
        
        curr_va += PGSIZE;
        curr_pa += PGSIZE;
    }
}

// 解除pgtbl中[va, va+len)区域的映射
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    // 验证参数合法性
    assert((va % PGSIZE) == 0, "vm_unmappages: va not aligned");
    assert(len > 0 && (len % PGSIZE) == 0, "vm_unmappages: invalid len");
    assert(va + len <= VA_MAX, "vm_unmappages: va out of range");

    uint64 curr_va = va;
    pte_t *pte;
    
    while (curr_va < va + len) {
        pte = vm_getpte(pgtbl, curr_va, false);
        if (!pte || !(*pte & PTE_V)) {
            curr_va += PGSIZE;
            continue;
        }

        // 如果需要释放物理页
        if (freeit) {
            uint64 pa = PTE_TO_PA(*pte);
            pmem_free(pa, false); 
        }

        // 清除页表项
        *pte = 0;
        curr_va += PGSIZE;
    }

    // 刷新TLB
    sfence_vma();
}

// 完成内核相关区域的页表映射
void kvm_init()
{
    // 分配内核页表根物理页
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(kernel_pgtbl != NULL, "kvm_init: page_alloc failed");
    memset(kernel_pgtbl, 0, PGSIZE);

    // 映射UART
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);

    // 映射CLINT
    vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE, 0xc000, PTE_R | PTE_W);

    // 映射PLIC
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);

    // 拆分映射内核代码段和数据段（按功能划分并设置对应权限）

    // 1. 映射内核代码段（KERNEL_BASE ~ KERNEL_DATA）
    // 仅包含指令，需要可读可执行权限（无写权限，保护代码不被篡改）
    uint64 kernel_code_size = (uint64)KERNEL_DATA - KERNEL_BASE;
    vm_mappages(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE, kernel_code_size, PTE_R | PTE_X);

    // 2. 映射内核数据段（KERNEL_DATA ~ ALLOC_BEGIN）
    // 包含全局变量、静态变量等，需要可读可写权限（无执行权限，避免数据被误执行）
    uint64 kernel_data_size = (uint64)ALLOC_BEGIN - (uint64)KERNEL_DATA;
    vm_mappages(kernel_pgtbl, (uint64)KERNEL_DATA, (uint64)KERNEL_DATA, kernel_data_size, PTE_R | PTE_W);

    // 映射可分配区域
    vm_mappages(kernel_pgtbl, (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN,
               (uint64)ALLOC_END - (uint64)ALLOC_BEGIN, PTE_R | PTE_W);

    // 映射trampoline
    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
    // 为所有进程分配和映射内核栈（每个进程2页=8KB）
    // 前提：内核初始化阶段物理内存无碎片，pmem_alloc(true)会按地址递增分配连续页
    for (int i = 0; i < N_PROC; i++) {
        // 关键修正：用in_kernel=true分配内核专属页，避免用户进程访问
        void *kstack_pa = pmem_alloc(true);
        if (kstack_pa == NULL) {
            panic("kvm_init: alloc kstack failed");
        }
        // 映射：VA=KSTACK(i) → PA=kstack_pa（起始页），大小2*PGSIZE
        // 依赖：pmem_alloc(true)返回的页与其下一个页（pa+PGSIZE）连续（无碎片时成立）
        vm_mappages(kernel_pgtbl, KSTACK(i), (uint64)kstack_pa, 2 * PGSIZE, PTE_R | PTE_W);
    }
}
// 每个CPU都需要调用, 从不使用页表切换到使用内核页表
void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

// 输出页表内容(for debug)
void vm_print(pgtbl_t pgtbl)
{
    // 顶级页表，次级页表，低级页表
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++)
    {
        pte = pgtbl_2[i];
        if (!((pte)&PTE_V))
            continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);

        for (int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if (!((pte)&PTE_V))
                continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_0);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++)
            {
                pte = pgtbl_0[k];
                if (!((pte)&PTE_V))
                    continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));
            }
        }
    }
}