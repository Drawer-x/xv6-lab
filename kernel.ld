OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
  . = 0x80000000;
  _start = .;

  _stext = .;
  .text : { *(.text .text.*) }
  _etext = .;

  .rodata : { *(.rodata .rodata.*) }

  _sdata = .;
  .data : { *(.data .data.*) }
  _edata = .;

  _sbss = .;
  .bss : { *(.bss .bss.*) *(COMMON) }
  _ebss = .;

  . = ALIGN(0x1000);
  PROVIDE(ALLOC_BEGIN = .);
  . += 0x01000000;   /* 16MB */
  PROVIDE(ALLOC_END = .);

  . = ALIGN(0x1000);
  trampoline = .;
  KEEP(*(.trampoline))

  /DISCARD/ : { *(.eh_frame) *(.comment) }
}
