#include "mod.h"

// 内核页表
static pgtbl_t kernel_pgtbl;

// ========== 内部小工具 ==========
static inline void *kalloc_pt_page() {
    void *p = pmem_alloc(true);
    assert(p != NULL, "kalloc_pt_page: pmem_alloc(true) failed");
    memset(p, 0, PGSIZE);
    return p;
}

static inline void vm_check_page_aligned(uint64 va, uint64 pa, uint64 len) {
    assert((va % PGSIZE) == 0, "va not page-aligned");
    assert((pa % PGSIZE) == 0, "pa not page-aligned");
    assert(len > 0, "len must be > 0");
    assert(va + len <= VA_MAX, "va+len exceeds VA_MAX");
}

// ========== 基础页表操作 ==========

// 逐级查找/分配页表项
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    pgtbl_t pt = pgtbl;
    for (int level = 2; level > 0; level--) {
        uint64 vpn = VA_TO_VPN(va, level);
        pte_t *pte = &pt[vpn];
        if (!(*pte & PTE_V)) {
            if (!alloc) return NULL;
            void *child = kalloc_pt_page();
            *pte = PA_TO_PTE((uint64)child) | PTE_V;
        } else {
            assert(PTE_CHECK(*pte), "vm_getpte: expected non-leaf pte");
        }
        pt = (pgtbl_t)PTE_TO_PA(*pte);
    }
    return &pt[VA_TO_VPN(va, 0)];
}

// 建立 [va,va+len) -> [pa,pa+len) 的映射
void vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
{
    vm_check_page_aligned(va, pa, len);
    uint64 end = va + len;
    while (va < end) {
        pte_t *pte = vm_getpte(pgtbl, va, true);
        assert(pte != NULL, "vm_mappages: getpte failed");
        assert(((*pte) & PTE_V) == 0, "vm_mappages: remap existing pte");
        *pte = PA_TO_PTE(pa) | perm | PTE_V;
        va += PGSIZE;
        pa += PGSIZE;
    }
}

// 解除映射，可选释放
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert((va % PGSIZE) == 0, "vm_unmappages: va not aligned");
    uint64 end = va + len;
    while (va < end) {
        pte_t *pte = vm_getpte(pgtbl, va, false);
        assert(pte && (*pte & PTE_V), "vm_unmappages: invalid pte");
        if (freeit) pmem_free(PTE_TO_PA(*pte), false);
        *pte = 0;
        va += PGSIZE;
    }
}

// ========== 内核页表初始化 ==========

void kvm_init()
{
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(kernel_pgtbl != NULL, "kvm_init: alloc failed");
    memset(kernel_pgtbl, 0, PGSIZE);

    extern char _stext[], _etext[];
    extern char _sdata[], _edata[];
    extern char ALLOC_BEGIN[], ALLOC_END[];
    extern char trampoline[];

    // 1. 内核代码段：RX
    vm_mappages(kernel_pgtbl,
                (uint64)_stext, (uint64)_stext,
                (uint64)(_etext - _stext),
                PTE_R | PTE_X);

    // 2. 数据段 + BSS 段：RW
    vm_mappages(kernel_pgtbl,
                (uint64)_sdata, (uint64)_sdata,
                (uint64)(_edata - _sdata),
                PTE_R | PTE_W);

    // 3. 可分配区：RW（恒等映射）
    vm_mappages(kernel_pgtbl,
                (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN,
                (uint64)(ALLOC_END - ALLOC_BEGIN),
                PTE_R | PTE_W);

    // 4. trampoline：RX（U/S 共享）
    uint64 tramp_va = (uint64)ALIGN_DOWN((uint64)trampoline, PGSIZE);
    vm_mappages(kernel_pgtbl,
                tramp_va, tramp_va,
                PGSIZE, PTE_R | PTE_X);

    // 5. 设备区（UART/CLINT/PLIC）——恒等映射 RW
#ifdef UART_BASE
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);
#endif
#ifdef CLINT_BASE
    vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);
#endif
#ifdef PLIC_BASE
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);
#endif

    printf("[kvm_init] kernel_pgtbl created.\n");
}

void kvm_inithart()
{
    w_satp(MAKE_SATP(kernel_pgtbl));
    sfence_vma();
}
