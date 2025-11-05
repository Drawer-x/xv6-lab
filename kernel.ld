OUTPUT_ARCH(riscv)
ENTRY(_entry)

SECTIONS
{
  /* 内核基址（OpenSBI 模式） */
  . = 0x80000000;

  /* -------- text -------- */
  PROVIDE(_stext = .);
  .text : { *(.text .text.*) }
  PROVIDE(_etext = .);

  /* -------- trampoline：独占一页 -------- */
  . = ALIGN(0x1000);
  PROVIDE(trampoline = .);
  .trampoline : { *(.trampoline) }

  /* ！！关键：把位置推进到下一页，避免 rodata/data 挤进同一页 */
  . = ALIGN(0x1000);

  /* -------- rodata（导出边界，方便映射） -------- */
  PROVIDE(_srodata = .);
  .rodata : { *(.rodata .rodata.*) }
  PROVIDE(_erodata = .);

  /* ----------------  (.data) ---------------- */
  . = ALIGN(0x1000);
  PROVIDE(_sdata = .);
  .data : {
    *(.data .data.*)
    *(.sdata .sdata.*)
  }
  PROVIDE(_edata = .);

  /* -------- bss -------- */
  _sbss = .;
  .bss : { *(.bss .bss.*) *(COMMON) }
  _ebss = .;

  /* -------- alloc region -------- */
  . = ALIGN(0x1000);
  PROVIDE(ALLOC_BEGIN = .);
  . += 0x01000000;   /* 16MB */
  PROVIDE(ALLOC_END = .);

  /DISCARD/ : { *(.eh_frame) *(.comment) }
}