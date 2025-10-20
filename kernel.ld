OUTPUT_ARCH(riscv)
ENTRY(_start)

/*
 * ECNU-OSLAB-2025 kernel.ld
 * - 支持 kvm.c 所需符号
 * - 修复 syntax error（移除 16M 写法）
 */

SECTIONS
{
  /* ---------------- 内核加载基地址 ---------------- */
  . = 0x80000000;

  /* ---------------- 代码段 (.text) ---------------- */
  _stext = .;
  .text : {
    *(.text .text.*)
  }
  _etext = .;

  /* ---------------- 只读数据 (.rodata) ---------------- */
  .rodata : {
    *(.rodata .rodata.*)
  }

  /* ---------------- 已初始化数据 (.data) ---------------- */
  _sdata = .;
  .data : {
    *(.data .data.*)
  }
  _edata = .;

  /* ---------------- 未初始化数据 (.bss) ---------------- */
  _sbss = .;
  .bss : {
    *(.bss .bss.*)
    *(COMMON)
  }
  _ebss = .;

  /* ---------------- 可分配物理内存区 ---------------- */
  . = ALIGN(0x1000);
  PROVIDE(ALLOC_BEGIN = .);
  . += 0x1000000;   /* 16MB */
  PROVIDE(ALLOC_END = .);

  /* ---------------- trampoline ---------------- */
  . = ALIGN(0x1000);
  trampoline = .;
  KEEP(*(.trampoline))

  /* ---------------- 丢弃无关段 ---------------- */
  /DISCARD/ : {
    *(.eh_frame)
    *(.comment)
  }
}
