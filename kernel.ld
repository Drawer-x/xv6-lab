OUTPUT_ARCH(riscv)
ENTRY(_entry)

SECTIONS
{
  /* Kernel is loaded at 0x80000000 on QEMU virt */
  . = 0x80000000;

  /* -------- text -------- */
  _stext = .;
  .text : {
    *(.text .text.*)
  }
  _etext = .;

  /* -------- rodata -------- */
  .rodata : {
    *(.rodata .rodata.*)
  }

  /* -------- data -------- */
  _sdata = .;
  .data : {
    *(.data .data.*)
  }
  _edata = .;

  /* -------- bss -------- */
  _sbss = .;
  .bss : {
    *(.bss .bss.*)
    *(COMMON)
  }
  _ebss = .;

  /* -------- allocatable physical memory -------- */
  . = ALIGN(0x1000);
  PROVIDE(ALLOC_BEGIN = .);
  . += 0x01000000; /* 16MB */
  PROVIDE(ALLOC_END = .);

  /* -------- trampoline -------- */
  . = ALIGN(0x1000);
  trampoline = .;
  KEEP(*(.trampoline));

  /* -------- discard unnecessary sections -------- */
  /DISCARD/ : {
    *(.eh_frame)
    *(.comment)
  }
}
