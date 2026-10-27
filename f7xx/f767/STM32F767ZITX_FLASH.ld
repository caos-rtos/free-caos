/* A.C. : DEFINIZIONI X TASK - ADATTATO PER STM32F767ZI (NUCLEO-F767ZI) */

/* Entry Point */
ENTRY(Reset_Handler)

/* === DTCM RAM layout (128 KB totali) ===
 * Su F7 la CCMRAM è sostituita dalla DTCMRAM.
 * Indirizzo base: 0x20000000
 *
 * Questa parte ha 128 KB di DTCM contro i 64 KB della F746 (DS11532 §3.5),
 * quindi le aree NON sono una copia della F746: la TLB passa da 4 KB a 8 KB
 * e l'arena PSP da 56 KB a 116 KB. Il rapporto fra le aree resta lo stesso.
 *
 * 0x2001FF00 - 0x20020000 : mcp_fault (256 B)
 * 0x2001F000 - 0x2001FF00 : msp_stack (3.75 KB)
 * 0x20002000 - 0x2001F000 : psp_stack (116 KB)
 * 0x20000000 - 0x20002000 : tlb_area  (8 KB)
 */

/* --- Aree DTCMRAM (128KB) --- */
PROVIDE(_msp_fault_size = 256);
PROVIDE(_dtcm_top       = 0x20020000); /* Fine della DTCM da 128KB */
PROVIDE(_msp_fault_top  = _dtcm_top);
PROVIDE(_msp_fault_base = _msp_fault_top - _msp_fault_size);

/* Stack MSP principale */
PROVIDE(_msp_stack_size = 4K - 256);
PROVIDE(_msp_stack_top  = _msp_fault_base);
PROVIDE(_msp_stack_base = _msp_stack_top - _msp_stack_size);

_estack = _msp_stack_top; /* 0x2001FF00 */

/* Stack PSP (per i Task) */
PROVIDE(_psp_stack_top  = _msp_stack_base);
PROVIDE(_psp_stack_size = 116K);
PROVIDE(_psp_stack_base = _psp_stack_top - _psp_stack_size);

/* Area TLB — 8K/180 (sizeof ca_thread_context_t) = 45 contesti, ma MAX_TASK in
 * objmap.h e' 25: il vincolo stringente non e' la TLB, e' questa arena PSP
 * (116K / STACK_MEDIUM 4K = 29 stack). Vedi la nota su MAX_TASK. */
PROVIDE(_tlb_area_base = 0x20000000);
PROVIDE(_tlb_area_size = 8K);
PROVIDE(_tlb_area_top  = _tlb_area_base + _tlb_area_size);

/* === Definizione memorie STM32F767ZI (DS11532 §3.3/3.5) ===
 * Flash 2 MB in SINGLE BANK (nDBANK=1, default di fabbrica): 2 MB lineari.
 * In dual bank i settori sarebbero dimezzati e la mappa diversa.
 */
MEMORY
{
  DTCMRAM   (rw) : ORIGIN = 0x20000000, LENGTH = 128K
  SRAM1     (rw) : ORIGIN = 0x20020000, LENGTH = 368K
  SRAM2     (rw) : ORIGIN = 0x2007C000, LENGTH = 16K
  FLASH     (rx) : ORIGIN = 0x08000000, LENGTH = 2048K /* Nucleo F767ZI ha 2MB Flash */
}

/* === Sezioni === */
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
    *(.text)
    *(.text*)
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* Dati inizializzati in SRAM1 (Accessibile da CPU e DMA) */
  _sidata = LOADADDR(.data);
  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >SRAM1 AT> FLASH

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
  } >SRAM1

  /* Pool dei buffer di rete (NET_BUF_COUNT=64 -> 128KB su F767).
   *
   * Dichiarata QUI, PRIMA di .heap, per lo stesso motivo di f4xx: .heap si
   * prende tutto cio' che resta di SRAM1 e NON consuma byte (_heap_end e'
   * un'assegnazione di simbolo, non un avanzamento di '.'), quindi il
   * contatore dopo .heap vale ancora _heap_start. Il pool deve stare prima,
   * perche' sia _heap_start a scavalcarlo.
   *
   * Senza questa sezione .net_pool e' una sezione ORFANA e la sua posizione
   * la decide l'euristica di ld, non lo script. Con gcc 10.3.1 cadeva bene
   * (subito dopo .bss, quindi prima di .heap: nessuna sovrapposizione,
   * verificato con un link di prova il 17/09), ma se ld l'avesse messa DOPO
   * .heap il pool sarebbe partito esattamente da _heap_start e 128KB di
   * heap sarebbero stati anche i buffer di rete. Dichiararla non cambia la
   * mappa - gli indirizzi sono identici - toglie solo la dipendenza
   * dall'euristica. */
  .net_pool (NOLOAD) : ALIGN(32)
  {
    *(.net_pool)
  } >SRAM1

  /* === Riserva aree in DTCMRAM === */
  .dtcm_reserved (NOLOAD) :
  {
    . = ORIGIN(DTCMRAM);
    _s_tlb = .;
    . = . + _tlb_area_size;
    _e_tlb = .;

    _s_psp_stack = .;
    . = . + _psp_stack_size;
    _e_psp_stack = .;
  } >DTCMRAM

  /* Heap su SRAM1 */
  .heap (NOLOAD) : ALIGN(32)
  {
    . = ALIGN(32);
    _heap_start = .;
    _heap_end = ORIGIN(SRAM1) + LENGTH(SRAM1);
  } >SRAM1

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }
}
