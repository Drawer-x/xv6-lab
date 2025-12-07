#include "mod.h"
#include "../fs/type.h"  // 为了使用 VIRTIO_BASE

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

    // 允许传入 NULL：表示使用内核页表（供 virtio.c 等内核场景使用）
    if (pgtbl == NULL)
        pgtbl = kernel_pgtbl;

    uint64 vpn2 = (va >> 30) & 0x1FF;
    uint64 vpn1 = (va >> 21) & 0x1FF;
    uint64 vpn0 = (va >> 12) & 0x1FF;

    pgtbl_t pt = pgtbl;
    // 下降两层：level=2 -> level=1
    for (int level = 2; level > 0; level--) {
        uint64 idx = (level == 2) ? vpn2 : vpn1;
        pte_t *pte = &pt[idx];

        if (!(*pte & PTE_V)) {
            if (!alloc) return NULL;

            uint64 pa = (uint64)pmem_alloc(true);
            if (!pa) return NULL;

            memset((void *)pa, 0, PGSIZE);
            *pte = PA_TO_PTE(pa) | PTE_V;   // 中间页表项：只置 V
        }

        if (!PTE_CHECK(*pte))   // 中间项必须是页表（R/W/X 均为 0）
            return NULL;

        pt = (pgtbl_t)PTE_TO_PA(*pte);
    }

    // 返回第 0 级页表项地址
    return &pt[vpn0];
}


// va从va开始, 以页为单位映射len字节的连续地址
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    assert((va % PGSIZE) == 0 && (pa % PGSIZE) == 0, "vm_mappages: not aligned");

    for (uint64 off = 0; off < len; off += PGSIZE) {
        pte_t *pte = vm_getpte(pgtbl, va + off, true);
        assert(pte != NULL, "vm_mappages: vm_getpte NULL");
        assert((*pte & PTE_V) == 0, "vm_mappages: remap");

        *pte = PA_TO_PTE(pa + off) | PTE_V | perm;  // 叶子项：V | perm
    }
}


// va从va开始, 以页为单位解除len字节的连续地址映射
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert((va % PGSIZE) == 0, "vm_unmappages: not aligned");

    for (uint64 off = 0; off < len; off += PGSIZE) {
        pte_t *pte = vm_getpte(pgtbl, va + off, false);
        assert(pte && (*pte & PTE_V), "vm_unmappages: pte invalid");

        if (freeit) {
            uint64 pa = (uint64)PTE_TO_PA(*pte);
            pmem_free(pa, true);
        }
        *pte = 0;
    }
}

// 内核页表初始化
void kvm_init()
{
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(kernel_pgtbl != NULL, "kvm_init: alloc kernel_pgtbl failed");
    memset(kernel_pgtbl, 0, PGSIZE);

    // 1. 映射内核text段（只读+可执行）
    uint64 kernel_code_size = (uint64)KERNEL_DATA - KERNEL_BASE;
    vm_mappages(kernel_pgtbl, KERNEL_BASE, KERNEL_BASE, kernel_code_size, PTE_R | PTE_X);

    // 2. 映射内核data/bss段（可读写）
    uint64 kernel_data_size = (uint64)ALLOC_BEGIN - (uint64)KERNEL_DATA;
    vm_mappages(kernel_pgtbl, (uint64)KERNEL_DATA, (uint64)KERNEL_DATA, kernel_data_size, PTE_R | PTE_W);

    // 3. 映射可分配区域（内核侧）
    vm_mappages(kernel_pgtbl, (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN,
               (uint64)ALLOC_END - (uint64)ALLOC_BEGIN, PTE_R | PTE_W);

    // 4. 映射 trampoline
    vm_mappages(kernel_pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

    // 5. 为每个进程映射内核栈（2页）
    for (int i = 0; i < N_PROC; i++) {
        void *kstack_pa = pmem_alloc(true);
        assert(kstack_pa != NULL, "kvm_init: alloc kstack failed");
        vm_mappages(kernel_pgtbl, KSTACK(i), (uint64)kstack_pa, 2 * PGSIZE, PTE_R | PTE_W);
    }


    // 映射常用 MMIO 寄存器
    vm_mappages(kernel_pgtbl, 0x10000000UL, 0x10000000UL, 0x00001000UL, PTE_R | PTE_W); // UART0: 4KB
    vm_mappages(kernel_pgtbl, 0x10001000UL, 0x10001000UL, 0x00001000UL, PTE_R | PTE_W); // VIRTIO: 4KB
    vm_mappages(kernel_pgtbl, 0x0C000000UL, 0x0C000000UL, 0x00400000UL, PTE_R | PTE_W); // PLIC: 4MB (覆盖 priority/enable/claim 等)
    vm_mappages(kernel_pgtbl, 0x02000000UL, 0x02000000UL, 0x00010000UL, PTE_R | PTE_W); // CLINT: 64KB (mtime/mtimecmp/软件中断)


}


// 每个CPU都需要调用, 从不使用页表切换到使用内核页表
void kvm_inithart()
{
    // 切换到内核页表，但此时不要开启中断
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();

    // 不要在这里 intr_on()！
    // 中断的开启放在 trap_kernel_inithart() 完成 PLIC/SIE 配置之后再开
}

// 打印页表（用于调试）
void vm_print(pgtbl_t pgtbl)
{
    pgtbl_t pgtbl_2 = pgtbl;
    printf("page table %p\n", (uint64)pgtbl_2);

    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++)
    {
        pte_t pte = pgtbl_2[i];
        if (!(pte & PTE_V))
            continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (1)");
        pgtbl_t pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);

        for (int j = 0; j < PGSIZE / sizeof(pte_t); j++)
        {
            pte = pgtbl_1[j];
            if (!(pte & PTE_V))
                continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (2)");
            pgtbl_t pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_0);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++)
            {
                pte = pgtbl_0[k];
                if (!(pte & PTE_V))
                    continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n", k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));
            }
        }
    }
}
