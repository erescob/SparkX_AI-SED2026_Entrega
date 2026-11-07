/* stm32f411rc_demo.ld — STM32F411RC (modo demo para Proteus; usa toda la flash (no guarda configuración)) */
ENTRY(Reset_Handler)
MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 256K
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}
_estack = ORIGIN(RAM) + LENGTH(RAM);
_stack_size = 8K;
SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : {
    *(.text*) *(.rodata*)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
  } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)); PROVIDE_HIDDEN(__preinit_array_end = .); } > FLASH
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))); KEEP(*(.init_array*)); PROVIDE_HIDDEN(__init_array_end = .); } > FLASH
  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(.fini_array*)); PROVIDE_HIDDEN(__fini_array_end = .); } > FLASH
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > RAM AT > FLASH
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > RAM
  _end = .;
  _heap_limit = _estack - _stack_size;
  ASSERT(_end <= _heap_limit, "RAM insuficiente")
}
