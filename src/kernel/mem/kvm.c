#include "mod.h"
// -----------------------------------------------------------------------------
// 内核页表
// -----------------------------------------------------------------------------
pgtbl_t kernel_pgtbl;
// -----------------------------------------------------------------------------
// 小工具
// -----------------------------------------------------------------------------
static inline void *kalloc_pt_page() {
    void *p = pmem_alloc(true);
    assert(p != NULL, "kalloc_pt_page: pmem_alloc(true) failed");
    memset(p, 0, PGSIZE);
    return p;
}

static inline void vm_check_page_aligned(uint64 va, uint64 pa, uint64 len) {
    assert((va % PGSIZE) == 0, "vm: va not page-aligned");
    assert((pa % PGSIZE) == 0, "vm: pa not page-aligned");
    assert(len > 0,            "vm: len must be > 0");
    assert(va + len <= VA_MAX, "vm: va+len exceeds VA_MAX");
}

// -----------------------------------------------------------------------------
// 基础页表操作
// -----------------------------------------------------------------------------

// 根据 pagetable 找到 va 对应的最低级 pte；alloc=true 时按需分配中间页表
pte_t *vm_getpte(pgtbl_t pgtbl, uint64 va, bool alloc)
{
    pgtbl_t pt = pgtbl;
    for (int level = 2; level > 0; level--) {
        uint64 vpn = VA_TO_VPN(va, level);
        pte_t *pte = &pt[vpn];
        if (!(*pte & PTE_V)) {
            if (!alloc) return NULL;
            void *child = kalloc_pt_page();
            *pte = PA_TO_PTE((uint64)child) | PTE_V;  // 中间级：仅有效位
        } else {
            // 中间级必须是指向下一级页表而非叶子
            assert(PTE_CHECK(*pte), "vm_getpte: non-leaf expected");
        }
        pt = (pgtbl_t)PTE_TO_PA(*pte);
    }
    return &pt[VA_TO_VPN(va, 0)];
}

// 在 pgtbl 中建立 [va,va+len) -> [pa,pa+len) 的映射（叶子 PTE 权限为 perm）
int vm_mappages(pgtbl_t pgtbl, uint64 va, uint64 pa, uint64 len, int perm)
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
    return 0;
}

// 解除 [va,va+len) 的映射；freeit=true 时释放对应物理页（通常用于用户页）
void vm_unmappages(pgtbl_t pgtbl, uint64 va, uint64 len, bool freeit)
{
    assert((va % PGSIZE) == 0, "vm_unmappages: va not aligned");
    uint64 end = va + len;
    while (va < end) {
        pte_t *pte = vm_getpte(pgtbl, va, false);
        assert(pte && (*pte & PTE_V), "vm_unmappages: invalid pte");
        if (freeit) {
            pmem_free(PTE_TO_PA(*pte), false);
        }
        *pte = 0;
        va += PGSIZE;
    }
}

// -----------------------------------------------------------------------------
// 内核页表初始化
// -----------------------------------------------------------------------------
void kvm_init()
{
    // 分配顶级页表
    kernel_pgtbl = (pgtbl_t)pmem_alloc(true);
    assert(kernel_pgtbl != NULL, "kvm_init: alloc kernel_pgtbl failed");
    memset(kernel_pgtbl, 0, PGSIZE);

    // 来自 kernel.ld 的段边界符号
    extern char _stext[], _etext[];            // .text
    extern char _srodata[], _erodata[];        // .rodata  <-- 新增
    extern char _sdata[], _ebss[];   // 注意 _ebss（不是 _edata）
    extern char ALLOC_BEGIN[], ALLOC_END[];    // 可分配物理内存区
    extern char trampoline[];                  // .trampoline 段起点（单页）

    // 1) .text ：RX
    vm_mappages(kernel_pgtbl,
                (uint64)_stext, (uint64)_stext,
                (uint64)(_etext - _stext),
                PTE_R | PTE_X);

    // 2) .rodata ：R  （与 .text 分离，避免落入 trampoline 页）
    vm_mappages(kernel_pgtbl,
                (uint64)_srodata, (uint64)_srodata,
                (uint64)(_erodata - _srodata),
                PTE_R);

    // 3) .data + .bss ：RW
    vm_mappages(kernel_pgtbl,
                (uint64)_sdata, (uint64)_sdata,
                (uint64)(_ebss - _sdata),
                PTE_R | PTE_W);
    
    // 4) 可分配区域（恒等映射）：RW
    vm_mappages(kernel_pgtbl,
                (uint64)ALLOC_BEGIN, (uint64)ALLOC_BEGIN,
                (uint64)(ALLOC_END - ALLOC_BEGIN),
                PTE_R | PTE_W);

    // 5) trampoline ：RX（独占一页；kernel.ld 保证其 4K 对齐）
    uint64 tramp_va = (uint64)ALIGN_DOWN((uint64)trampoline, PGSIZE);
    vm_mappages(kernel_pgtbl,
                tramp_va, tramp_va,
                PGSIZE,
                PTE_R | PTE_X);

    // 6) 设备 MMIO（恒等映射）：RW
#ifdef UART_BASE
    vm_mappages(kernel_pgtbl, UART_BASE, UART_BASE, PGSIZE, PTE_R | PTE_W);
#endif
#ifdef CLINT_BASE
    // CLINT 常用 0x200000 ~ 0x200000+0x10000（根据平台调整）
    vm_mappages(kernel_pgtbl, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W);
#endif
#ifdef PLIC_BASE
    // PLIC 常用较大映射窗口（此处给 4MB 覆盖）
    vm_mappages(kernel_pgtbl, PLIC_BASE, PLIC_BASE, 0x400000, PTE_R | PTE_W);
#endif

    printf("[kvm_init] kernel_pgtbl ready. text[%p,%p) rodata[%p,%p) data..bss[%p,%p) tramp[%p]\n",
       _stext, _etext, _srodata, _erodata, _sdata, _ebss, trampoline);
    // 在 kvm_init 中映射 ALLOC_BEGIN 后添加：
    printf("[kvm_init] ALLOC mapped: va=[%p, %p), pa=[%p, %p), perm=R|W\n",
       ALLOC_BEGIN, ALLOC_END,
       ALLOC_BEGIN, ALLOC_END);
}

// /* 初始化当前HART的内核页表和栈 */
// void kvm_inithart() {
//     // 1. 切换到内核页表
//     w_satp(MAKE_SATP(kernel_pgtbl));
//     sfence_vma();  // 刷新TLB

//     // 2. 分配内核栈并初始化mscratch寄存器（解决空指针访问问题）new
//     char *kstack = (char*)pmem_alloc(true);  // 从内核区域分配一页作为栈
//     assert(kstack != NULL, "kvm_inithart: failed to alloc kernel stack");
//     uint64 stack_top = (uint64)kstack + PGSIZE;  // 栈顶地址（RISC-V栈向下生长）
//     w_mscratch(stack_top);  // mscratch存储内核栈顶
//     printf("[kvm_inithart] hart initialized. stack_top=%p\n", stack_top);
// }
void kvm_inithart() {
  printf("[kvm_inithart] start\n");  // 第一步打印
  // 1. 切换到内核页表
  w_satp(MAKE_SATP(kernel_pgtbl));
  printf("[kvm_inithart] satp set\n");  // 确认页表切换前的打印
  sfence_vma();
  printf("[kvm_inithart] tlb flushed\n");  // 确认TLB刷新

  // 2. 分配内核栈
  char *kstack = (char*)pmem_alloc(true);
  assert(kstack != NULL, "kvm_inithart: alloc stack failed");
  printf("[kvm_inithart] stack allocated at %p\n", kstack);  // 确认栈分配

  uint64 stack_top = (uint64)kstack + PGSIZE;
  w_mscratch(stack_top);
  // 读写栈顶下方的地址（测试栈访问）
  *(uint64*)(stack_top - 8) = 0x12345678;  // 向栈压入一个值
  uint64 test = *(uint64*)(stack_top - 8);  // 从栈读出
  if (test != 0x12345678) {
    panic("stack access failed");  // 若失败，打印错误
  }
  printf("[kvm_inithart] stack test passed. stack_top=%p\n", stack_top);
  printf("[kvm_inithart] hart initialized. stack_top=%p\n", stack_top);
}

// -----------------------------------------------------------------------------
// Debug：输出页表内容（三级页表）
// -----------------------------------------------------------------------------
void vm_print(pgtbl_t pgtbl)
{
    pgtbl_t pgtbl_2 = pgtbl, pgtbl_1 = NULL, pgtbl_0 = NULL;
    pte_t pte;

    printf("level-2 pgtbl: pa = %p\n", pgtbl_2);
    for (int i = 0; i < PGSIZE / sizeof(pte_t); i++) {
        pte = pgtbl_2[i];
        if (!(pte & PTE_V)) continue;
        assert(PTE_CHECK(pte), "vm_print: pte check fail (L2)");
        pgtbl_1 = (pgtbl_t)PTE_TO_PA(pte);
        printf(".. level-1 pgtbl %d: pa = %p\n", i, pgtbl_1);

        for (int j = 0; j < PGSIZE / sizeof(pte_t); j++) {
            pte = pgtbl_1[j];
            if (!(pte & PTE_V)) continue;
            assert(PTE_CHECK(pte), "vm_print: pte check fail (L1)");
            pgtbl_0 = (pgtbl_t)PTE_TO_PA(pte);
            printf(".. .. level-0 pgtbl %d: pa = %p\n", j, pgtbl_0);

            for (int k = 0; k < PGSIZE / sizeof(pte_t); k++) {
                pte = pgtbl_0[k];
                if (!(pte & PTE_V)) continue;
                assert(!PTE_CHECK(pte), "vm_print: pte check fail (L0 leaf)");
                printf(".. .. .. pte %d: pa = %p flags = 0x%lx\n",
                       k, (uint64)PTE_TO_PA(pte), (uint64)PTE_FLAGS(pte));
            }
        }
    }
}
