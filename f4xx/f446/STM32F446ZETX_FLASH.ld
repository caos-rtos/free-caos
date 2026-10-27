/* A.C. : DEFINIZIONI X TASK */



/* Entry Point */

ENTRY(Reset_Handler)

/* === IMPORTANTE: il F446 NON ha CCM RAM ===
 * Questo script derivava da quello del F407, che mette tlb_area, psp_stack e
 * msp_stack in CCMRAM a 0x10000000. Sul F446 quell'indirizzo NON ESISTE:
 * stm32f446xx.h non definisce CCMDATARAM_BASE (il F407 si', a riga 907).
 * Il F446ZE ha solo SRAM1 112 KB + SRAM2 16 KB CONTIGUE = 128 KB a 0x20000000.
 *
 * Le tre aree sono quindi state spostate in cima alla RAM normale. Sono
 * (NOLOAD) e riservate esplicitamente, quindi .data/.bss/heap partono dopo.
 *
 * === Layout RAM (128 KB totali) ===
 * Indirizzi crescenti; gli stack crescono verso il BASSO dentro la loro area:
 *
 * 0x20000000 - 0x20001000 : tlb_area  (4 KB)    <- dati statici (non stack)
 * 0x20001000 - 0x2000D000 : psp_stack (48 KB)   <- area stack dei task (PSP)
 * 0x2000D000 - 0x2000EF00 : msp_stack (8K-256)  <- MSP, _estack=top per main
 * 0x2000EF00 - 0x2000F000 : msp_fault (256 B)   <- MSP fault per le eccezioni
 * 0x2000F000 - 0x20020000 : .data/.bss/.net_pool/heap (64 KB)
 *
 * PSP a 48 KB e non 52 KB come sul F407: li' i 64 KB di CCM erano dedicati e
 * la RAM restava tutta libera, qui le due cose condividono i 128 KB.
 * 48 KB coprono comunque MAX_TASK=23 con STACK_MEDIUM (2 KB) = 46 KB.
 */

/* --- Base del blocco kernel, in cima alla RAM --- */
PROVIDE(_kernel_ram_base = ORIGIN(RAM));                     /* 0x20000000 */

/* Area TLB (assicurati che non si sovrapponga!) */
PROVIDE(_tlb_area_base = _kernel_ram_base);                  /* 0x20000000 */
PROVIDE(_tlb_area_size = 4K);
PROVIDE(_tlb_area_top  = _tlb_area_base + _tlb_area_size);   /* 0x20001000 */

/* Stack PSP (inizia dove finisce la TLB) */
PROVIDE(_psp_stack_base = _tlb_area_top);                    /* 0x20001000 */
PROVIDE(_psp_stack_size = 48K);
PROVIDE(_psp_stack_top  = _psp_stack_base + _psp_stack_size);/* 0x2000D000 */

/* Stack MSP principale (ridotto di 256 B per il fault stack) */
PROVIDE(_msp_stack_base = _psp_stack_top);                   /* 0x2000D000 */
PROVIDE(_msp_stack_size = 8K - 256);
PROVIDE(_msp_stack_top  = _msp_stack_base + _msp_stack_size);/* 0x2000EF00 */

/* Stack fault dedicato (256 B sopra l'MSP) */
PROVIDE(_msp_fault_base = _msp_stack_top);                   /* 0x2000EF00 */
PROVIDE(_msp_fault_size = 256);
PROVIDE(_msp_fault_top  = _msp_fault_base + _msp_fault_size);/* 0x2000F000 */

_estack = _msp_stack_top; /* 0x2000EF00 */


/* === Definizione memorie ===
 * Nessuna CCMRAM: il F446 non ce l'ha. */
MEMORY
{
  RAM       (rw) : ORIGIN = 0x20000000, LENGTH = 128K
  FLASH     (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
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

  /* === Riserva esplicita delle aree kernel, IN TESTA alla RAM ===
   * Deve stare PRIMA di .data/.bss: e' (NOLOAD) e ancorata a ORIGIN(RAM),
   * cosi' il resto della RAM viene allocato di seguito senza sovrapporsi. */
  .kernel_reserved (NOLOAD) :
  {
    . = ALIGN(32);

    /* tlb_area: 4 KB per dati critici */
    _s_tlb = .;
    . = . + _tlb_area_size;
    _e_tlb = .;

    /* psp_stack: 48 KB per stack dei task */
    _s_psp_stack = .;
    . = . + _psp_stack_size;
    _e_psp_stack = .;

    /* msp_stack + msp_fault: riservati esplicitamente.
     * Sul F407 stavano in CCMRAM e bastava "lasciare lo spazio"; qui la RAM
     * e' condivisa con .data/.bss, quindi lo spazio va occupato davvero,
     * altrimenti l'MSP crescerebbe dentro le variabili. */
    _s_msp_stack = .;
    . = . + _msp_stack_size + _msp_fault_size;
    _e_msp_stack = .;
  } >RAM

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

  /* Net buffer pool (NET_BUF_COUNT=8 → 16KB, come il F407: stessa RAM 128K).
   * Su questa board non c'e' PHY (MCU_NO_ETH): la sezione resta vuota a meno
   * che NET non giri su USB-NCM — ed e' proprio quel caso (ex55, ex69-83) che
   * con 16 buffer sforava la RAM. */
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

