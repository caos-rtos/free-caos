/* Linker script for STM32U575xx
 * FLASH: 2MB @ 0x08000000
 * RAM:   768KB contiguous (SRAM1 192K + SRAM2 64K + SRAM3 512K) @ 0x20000000
 *
 * RAM layout (768KB = 0xC0000):
 *   0x200BFFFF (top)
 *   0x200BFF00 - 0x200C0000 : msp_fault (256 B)
 *   0x200BE000 - 0x200BFF00 : msp_stack (8KB - 256B)
 *   0x200A1000 - 0x200BE000 : psp_stack (116 KB)
 *   0x2009D000 - 0x200A1000 : tlb_area  (16 KB)
 *   0x20000000 - 0x2009D000 : data/bss/heap (628 KB)
 */

_estack = 0x200BFF00;

_tlb_area_base = 0x2009D000;
_tlb_area_size = 0x4000;
_s_tlb         = _tlb_area_base;
_e_tlb         = 0x200A1000;

_s_psp_stack    = 0x200A1000;
_psp_stack_size = 0x1D000;
_e_psp_stack    = 0x200BE000;
_psp_stack_top  = _e_psp_stack;
_psp_stack_base = _s_psp_stack;

_msp_stack_base = 0x200BE000;
_msp_stack_top  = 0x200BFF00;
_msp_fault_base = 0x200BFF00;
_msp_fault_top  = 0x200C0000;

ENTRY(Reset_Handler)

MEMORY
{
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 628K
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text) *(.text*) *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP(*(.init)) KEEP(*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata) *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { . = ALIGN(4); *(.ARM.extab* .gnu.linkonce.armextab.*) . = ALIGN(4); } >FLASH
  .ARM        : { . = ALIGN(4); __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; . = ALIGN(4); } >FLASH

  .preinit_array (READONLY) : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
  .init_array    (READONLY) : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  .fini_array    (READONLY) : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4); _sdata = .;
    *(.data) *(.data*) *(.RamFunc) *(.RamFunc*)
    . = ALIGN(4); _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4); _sbss = .; __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON)
    . = ALIGN(4); _ebss = .; __bss_end__ = _ebss;
  } >RAM

  /* Net buffer pool (NET_BUF_COUNT=32 → 64KB) in RAM normale.
     U5 = Cortex-M33 no D-cache + USB slave-mode (CPU copy, no DMA sul pool):
     nessuna config MPU NonCacheable necessaria (a differenza di H7/D2 SRAM). */
  .net_pool (NOLOAD) : ALIGN(32)
  {
    *(.net_pool)
  } >RAM

  .heap (NOLOAD) : ALIGN(32)
  {
    _heap_start = .;
    . = ORIGIN(RAM) + LENGTH(RAM);
    _heap_end = .;
  } >RAM

  /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
  .ARM.attributes 0 : { *(.ARM.attributes) }
}
