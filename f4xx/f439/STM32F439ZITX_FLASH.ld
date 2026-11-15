/* A.C. : DEFINIZIONI X TASK */



/* Entry Point */

ENTRY(Reset_Handler)

/* === CCMRAM layout (64 KB totali) ===
 * Indirizzi decrescenti (stack crescono verso il basso):
 *
 * 0x1000FF00 - 0x10010000 : mcp_fault (256 B)   ← MSP fault x eccezoni, 
 * 0x1000F000 - 0x1000FF00 : msp_stack (8-256 KB)   ← MSP stack, _estack=top per main
 * 0x10001000 - 0x1000F000 : psp_stack (52 KB)  ← area per stack task (PSP)
 * 0x10000000 - 0x10001000 : tlb_area  (4 KB)   ← dati statici (non stack)
 */



/* --- Aree CCMRAM --- */

/* Stack fault dedicato (256 B alla sommità della CCMRAM) */
PROVIDE(_msp_fault_size = 256);
PROVIDE(_msp_fault_top  = ORIGIN(CCMRAM) + LENGTH(CCMRAM));  /* 0x10010000 */
PROVIDE(_msp_fault_base = _msp_fault_top - _msp_fault_size);  /* 0x1000FF00 */

/* Stack MSP principale (ridotto di 256 B) */
PROVIDE(_msp_stack_size = 8K - 256);
PROVIDE(_msp_stack_top  = _msp_fault_base);                  /* 0x1000FF00 */
PROVIDE(_msp_stack_base = _msp_stack_top - _msp_stack_size); /* 0x1000F000 */


_estack = _msp_stack_top; /* 0x1000FF00 */


/* Stack PSP (inizia dove finisce MSP) */
PROVIDE(_psp_stack_top  = _msp_stack_base);                  /* 0x1000F000 */
PROVIDE(_psp_stack_size = 52K);
PROVIDE(_psp_stack_base = _psp_stack_top - _psp_stack_size); /* 0x10001000 */

/* Area TLB (assicurati che non si sovrapponga!) */
PROVIDE(_tlb_area_base = ORIGIN(CCMRAM));                    /* 0x10000000 */
PROVIDE(_tlb_area_size = 4K);
PROVIDE(_tlb_area_top  = _tlb_area_base + _tlb_area_size);   /* 0x10001000 */



/* === Definizione memorie === */
MEMORY
{
  CCMRAM    (rw) : ORIGIN = 0x10000000, LENGTH = 64K
  RAM       (rw) : ORIGIN = 0x20000000, LENGTH = 192K   /* SRAM1+SRAM2+SRAM3, contiguous (DS9484 3.6) */
  FLASH     (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K  /* STM32F439ZI: 2 MB, dual bank */
}

/* === Sezioni === */
SECTIONS
{
  /* Vettore di interrupt */
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  /* Codice */
  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  /* Dati costanti */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* Eccezioni ed informazioni di runtime */
  .ARM.extab :
  {
    . = ALIGN(4);
    *(.ARM.extab* .gnu.linkonce.armextab.*)
    . = ALIGN(4);
  } >FLASH

  .ARM :
  {
    . = ALIGN(4);
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
    . = ALIGN(4);
  } >FLASH

.preinit_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .init_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
    . = ALIGN(4);
  } >FLASH

  .fini_array (READONLY) :
  {
    . = ALIGN(4);
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
    . = ALIGN(4);
  } >FLASH

  /* Dati inizializzati in RAM normale */
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH



  /* BSS in RAM normale */
  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  /* === Riserva esplicita aree in CCMRAM === */
  .ccm_reserved (NOLOAD) :
  {
    . = ORIGIN(CCMRAM);

    /* tlb_area: 4 KB per dati critici */
    _s_tlb = .;
    . = . + _tlb_area_size;
    _e_tlb = .;

    /* psp_stack: 52 KB per stack dei task */
    _s_psp_stack = .;
    . = . + _psp_stack_size;
    _e_psp_stack = .;

    /* NOTA: msp_stack NON viene allocato qui.
     * È gestito dal runtime: MSP parte da _estack e cresce verso il basso.
     * Lasciamo lo spazio libero riservandolo implicitamente tramite layout.
     */
  } >CCMRAM



  /* Net buffer pool (NET_BUF_COUNT=32 → 64KB on F439) */
  .net_pool (NOLOAD) : ALIGN(32)
  {
    *(.net_pool)
  } >RAM

  .heap (NOLOAD) : ALIGN(32)
  {
    . = ALIGN(8);
    _heap_start = .;
    . = (ORIGIN(RAM) + LENGTH(RAM)) - _heap_start;
    _heap_end = .;
    
  } >RAM

  /* Rimuovi metadati inutili */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}

