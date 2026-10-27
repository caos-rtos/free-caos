/* A.C. : LINKER SCRIPT FOR STM32H563xx */
/* 2MB FLASH, 640KB RAM (no CCM) */

ENTRY(Reset_Handler)

/* === RAM layout (640 KB = 0xA0000) ===
 * H563 non ha CCM, ultimi 140KB usati come area task:
 *
 * 0x200A0000  (top RAM)
 * 0x2009FF00 - 0x200A0000 : msp_fault (256 B)
 * 0x2009E000 - 0x2009FF00 : msp_stack (8KB - 256B)
 * 0x20081000 - 0x2009E000 : psp_stack (116 KB)
 * 0x2007D000 - 0x20081000 : tlb_area  (16 KB)
 * 0x20000000 - 0x2007D000 : data/bss/heap (500 KB)
 */

/* === Valori assoluti per evitare dipendenze circolari === */
_estack = 0x2009FF00;

/* TLB area */
_tlb_area_base = 0x2007D000;
_tlb_area_size = 0x4000;      /* 16KB */
_s_tlb = _tlb_area_base;
_e_tlb = 0x20081000;

/* PSP stacks */
_s_psp_stack = 0x20081000;
_psp_stack_size = 0x1D000;    /* 116KB */
_e_psp_stack = 0x2009E000;
_psp_stack_top = _e_psp_stack;
_psp_stack_base = _s_psp_stack;

/* MSP stack */
_msp_stack_base = 0x2009E000;
_msp_stack_top = 0x2009FF00;
_msp_fault_base = 0x2009FF00;
_msp_fault_top = 0x200A0000;

/* === Definizione memorie === */
/* Flash window of the image. Default: the whole flash, as always. An image
 * for a bootloader slot (workspace image.cmake, workspace/app_2/doc)
 * is linked with -Wl,--defsym=__caos_flash_origin=... and __caos_flash_length. */
__caos_flash_origin = DEFINED(__caos_flash_origin) ? __caos_flash_origin : 0x08000000;
__caos_flash_length = DEFINED(__caos_flash_length) ? __caos_flash_length : 2048K;

MEMORY
{
  RAM       (rwx) : ORIGIN = 0x20000000, LENGTH = 352K
  /* Tutta la memoria toccata dal DMA ETH, isolata dal resto.
   * ORIGIN allineato a 32KB: e' il vincolo di una region MPU ARMv8-M, se un
   * domani servisse darle attributi propri (l'H7 ha gia' questa struttura con
   * ETH_SRAM3). Prima .net_pool e .eth_dma stavano in >RAM fra .bss e .heap:
   * il loro indirizzo cambiava a ogni variabile aggiunta e non c'era nessun
   * confine fra i buffer del DMA e heap/stack. Contiene .net_pool (128KB) +
   * .eth_dma (128B); 148KB lasciano margine per NET_BUF_COUNT piu' alto. */
  ETH_RAM   (rw)  : ORIGIN = 0x20058000, LENGTH = 148K
  FLASH     (rx)  : ORIGIN = __caos_flash_origin, LENGTH = __caos_flash_length
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

  /* Dati inizializzati */
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

  /* BSS */
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

  /* Net buffer pool + descrittori ETH DMA, in ETH_RAM (0x20058000).
   * H5 (Cortex-M33) non ha D-cache sulla SRAM interna: ETH DMA e CPU vedono
   * dati coerenti senza cache maintenance, quindi zero-copy come H7 senza
   * dominio D2 ne' MPU. L'isolamento qui NON e' per la cache: serve a dare
   * ai ring un indirizzo FISSO e un confine, invece di farli galleggiare
   * fra .bss e .heap a ogni build.
   * ALIGN(512) su entrambe: i burst del DMA sono a beat allineati (AAL=1) e
   * un descrittore o un buffer a cavallo di una pagina costringe il master a
   * spezzare il burst. */
  .net_pool (NOLOAD) : ALIGN(512)
  {
    *(.net_pool)
  } >ETH_RAM

  .eth_dma (NOLOAD) : ALIGN(512)
  {
    *(.eth_dma)
  } >ETH_RAM

  /* Heap: tutto il resto di RAM, che ora finisce a 0x20058000 dove comincia
   * ETH_RAM (prima arrivava a 0x2007D000). L'heap NON confina piu' con i
   * buffer del DMA.
   * (resto della RAM fino a 352KB).
   * ALIGN(32): la guardia MPU heap-start (ARMv8-M) allinea _heap_start a 32
   * verso il BASSO (base &= ~0x1F). Con heap allineato solo a 8, la guardia
   * RO sconfinava negli ultimi ≤24 byte di .bss (es. s_rca/s_block_count del
   * driver SDIO) → DACCVIOL alla scrittura. Allineando l'heap a 32, la guardia
   * protegge i primi 32 byte DELL'heap, senza invadere .bss. */
  .heap (NOLOAD) : ALIGN(32)
  {
    _heap_start = .;
    . = ORIGIN(RAM) + LENGTH(RAM);
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
