#include "mod.h"

// 内核页表（顶级页表，SV39）
static pgtbl_t kernel_pgtbl;

/* --------------------------------- 内部工具 --------------------------------- */

static inline int is_page_aligned(uint64 x) { return (x & (PGSIZE - 1)) == 0; }

static inline int in_user_pgtbl(pgtbl_t pgtbl) {
    return pgtbl != kernel_pgtbl; // 不是内核页表就视为“用户页表”
}

/* 在 pgtbl 中找到 va 对应的最低级 PTE 指针；alloc=true 时按需分配中间页表 */
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    if (va >= VA_MAX) return 0;

    pgtbl_t cur = pgtbl;

    // 依次走 level-2, level-1 两级，拿到 level-0 页表
    for (int level = 2; level > 0; level--) {
        int idx = (int)VA_TO_VPN(va, level);
        pte_t *pte = &cur[idx];
        if (*pte & PTE_V) {
            // 已存在：应指向下一层页表
            assert(PTE_CHECK(*pte), "vm_getpte: non-leaf pte has R/W/X");
            cur = (pgtbl_t)PTE_TO_PA(*pte);
        } else {
            if (!alloc) return 0;
            // 分配一个新的页表页（内核物理页，作为页表使用）
            void *page = pmem_alloc(true);
            memset(page, 0, PGSIZE);
            *pte = PA_TO_PTE(page) | PTE_V;   // 仅V=1，表示页表页
            cur  = (pgtbl_t)page;
        }
    }

    // level-0
    int idx0 = (int)VA_TO_VPN(va, 0);
    return &cur[idx0];
}

/* 在 pgtbl 中建立 [va, va+len) -> [pa, pa+len) 的页映射（页粒度） */
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    assert(is_page_aligned(va) && is_page_aligned(pa), "vm_mappages: not aligned");
    assert(len > 0 && va + len <= VA_MAX, "vm_mappages: bad range");

    uint64 a = va;
    uint64 p = pa;
    uint64 last = va + len;

    while (a < last) {
        pte_t *pte = vm_getpte(pgtbl, a, true);
        assert(pte != 0, "vm_mappages: getpte fail");
        if (*pte & PTE_V) {
            panic("vm_mappages: remap");
        }
        *pte = PA_TO_PTE(p) | PTE_V | (perm & 0x3FF);
        a += PGSIZE;
        p += PGSIZE;
    }
}

/* 解除 pgtbl 中 [va, va+len) 的映射；freeit=true 时释放对应物理页 */
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert(is_page_aligned(va), "vm_unmappages: va not aligned");
    assert(len > 0 && va + len <= VA_MAX, "vm_unmappages: bad range");

    uint64 a = va;
    uint64 last = va + len;
    int to_kernel = (pgtbl == kernel_pgtbl);

    while (a < last) {
        pte_t *pte = vm_getpte(pgtbl, a, false);
        assert(pte && (*pte & PTE_V), "vm_unmappages: not mapped");
        if (!PTE_CHECK(*pte)) {
            // 叶子PTE
            uint64 pa = (uint64)PTE_TO_PA(*pte);
            if (freeit) {
                pmem_free(pa, to_kernel ? true : false);
            }
            *pte = 0;
        } else {
            // 指向下一级页表（这里一般不应出现，因为我们拿的是 level-0）
            panic("vm_unmappages: unexpected non-leaf");
        }
        a += PGSIZE;
    }
}

/* ------------------------------- 内核页表初始化 ------------------------------- */

void kvm_init()
{
    // 分配并清空顶级页表
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    memset(kernel_pgtbl, 0, PGSIZE);

    // 1) 映射 UART（设备恒等映射，RW）
    //    你已在 lib/type.h 看到 UART_BASE。
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);

    // 2) 若有 CLINT/PLIC 的基址与尺寸，可按需映射（示例保留）
    // vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE, CLINT_SIZE, PTE_R | PTE_W);
    // vm_mappages(kernel_pgtbl, PLIC_BASE,  PLIC_BASE,  PLIC_SIZE,  PTE_R | PTE_W);

    // 3) 内核 .text （可执行+只读）
    uint64 ktext_l = KERNEL_BASE;
    uint64 ktext_r = (uint64)KERNEL_DATA;
    if (ktext_r > ktext_l) {
        vm_mappages(kernel_pgtbl, ktext_l, ktext_l, ktext_r - ktext_l, PTE_R | PTE_X);
    }

    // 4) 内核数据/rodata/bss 区域（KERNEL_DATA ~ ALLOC_BEGIN）：RW
    uint64 kdata_l = (uint64)KERNEL_DATA;
    uint64 kdata_r = (uint64)ALLOC_BEGIN;
    if (kdata_r > kdata_l) {
        vm_mappages(kernel_pgtbl, kdata_l, kdata_l, kdata_r - kdata_l, PTE_R | PTE_W);
    }

    // 5) 可分配区域（ALLOC_BEGIN ~ ALLOC_END）：RW（内核态访问）
    uint64 pool_l = (uint64)ALLOC_BEGIN;
    uint64 pool_r = (uint64)ALLOC_END;
    if (pool_r > pool_l) {
        vm_mappages(kernel_pgtbl, pool_l, pool_l, pool_r - pool_l, PTE_R | PTE_W);
    }
}

/* 每个 CPU 上切换到内核页表 */
void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}

/* ------------------------------- 页表打印(已给) ------------------------------- */

void vm_print(pgtbl_t pgtbl)
{
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
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_2);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++)
            {
                pte = pgtbl_0[k];
                if (!((pte)&PTE_V))
                    continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (3)");
                printf(".. .. .. physical page %d: pa = %p flags = %d\n",
                       k, (uint64)PTE_TO_PA(pte), (int)PTE_FLAGS(pte));
            }
        }
    }
}
