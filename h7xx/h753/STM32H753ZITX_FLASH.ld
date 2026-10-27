/* A.C. : LINKER SCRIPT FOR STM32H753xx */
/* 2MB FLASH, 512KB AXI-SRAM + 128KB DTCM */

ENTRY(Reset_Handler)

/* === DTCM layout (128 KB = 0x20000) @ 0x20000000 ===
 * DTCM usato per task stacks (come CCM su F407):
 *
 * 0x20020000  (top DTCM)
 * 0x2001FF00 - 0x20020000 : msp_fault (256 B)
 * 0x2001E000 - 0x2001FF00 : msp_stack (8KB - 256B)
 * 0x20004000 - 0x2001E000 : psp_stack (104 KB)
 * 0x20000000 - 0x20004000 : tlb_area  (16 KB)
 *
 * === AXI-SRAM (512 KB) @ 0x24000000 ===
 * Data/BSS/Heap
 */

/* === Valori assoluti per evitare dipendenze circolari === */
_estack = 0x2001FF00;

/* TLB area in DTCM */
_tlb_area_base = 0x20000000;
_tlb_area_size = 0x4000;      /* 16KB */
_s_tlb = _tlb_area_base;
_e_tlb = 0x20004000;

/* PSP stacks in DTCM */
_s_psp_stack = 0x20004000;
_psp_stack_size = 0x1A000;    /* 104KB */
_e_psp_stack = 0x2001E000;
_psp_stack_top = _e_psp_stack;
_psp_stack_base = _s_psp_stack;

/* MSP stack in DTCM */
_msp_stack_base = 0x2001E000;
_msp_stack_top = 0x2001FF00;
_msp_fault_base = 0x2001FF00;
_msp_fault_top = 0x20020000;

/* === Definizione memorie === */
/* AXI SRAM (D1): 0x24000000 - 0x2407FFFF = 512KB (RM0433 §2.4)       */
/* D2  SRAM     : SRAM1 128K + SRAM2 128K + SRAM3 32K = 288KB          */
/*   NET_SRAM1 : 0x30000000, 128KB — net buffer pool 64×2KB (MPU NCacheable) */
/*   USB_SRAM  : 0x30020000, 128KB — USB DMA (SRAM1 upper + SRAM2)         */
/*   ETH_SRAM3 : 0x30040000,  32KB — ETH DMA descriptors + ring bufs       */
/* Flash window of the image. Default: the whole flash, as always. An image
 * for a bootloader slot (workspace image.cmake, workspace/app_2/doc)
 * is linked with -Wl,--defsym=__caos_flash_origin=... and __caos_flash_length. */
__caos_flash_origin = DEFINED(__caos_flash_origin) ? __caos_flash_origin : 0x08000000;
__caos_flash_length = DEFINED(__caos_flash_length) ? __caos_flash_length : 2048K;

MEMORY
{
  FLASH     (rx)  : ORIGIN = __caos_flash_origin, LENGTH = __caos_flash_length
  RAM       (rwx) : ORIGIN = 0x24000000, LENGTH = 512K
  NET_SRAM1 (rw)  : ORIGIN = 0x30000000, LENGTH = 128K
  USB_SRAM  (rw)  : ORIGIN = 0x30020000, LENGTH = 128K
  ETH_SRAM3 (rw)  : ORIGIN = 0x30040000, LENGTH = 32K
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

  /* Heap (resto della RAM) */
  .heap (NOLOAD) : ALIGN(32)
  {
    _heap_start = .;
    . = ORIGIN(RAM) + LENGTH(RAM);
    _heap_end = .;
  } >RAM

  /* Net buffer pool in D2 SRAM1 (0x30000000, 128KB).
   * MPU region NET_MPU_REGION (see objmap.h) configured as Normal NCacheable by
   * ca_net_buf_pool_init() — ETH DMA and CPU see coherent data with no
   * cache management required. */
  .net_pool (NOLOAD) :
  {
    *(.net_pool)
  } >NET_SRAM1

  /* USB DMA buffers in D2 SRAM2 (0x30020000, 128KB).
   * Regione non cacheable (D2 domain) — accessibile da DWC2 AHB master
   * senza cache maintenance. Layout: setup(24) + ep0_rx/tx(64+64) +
   * epn_rx/tx(8×512 + 8×512) × 2 istanze = ~16.7 KB */
  .usb_dma (NOLOAD) :
  {
    *(.usb_dma)
  } >USB_SRAM

  /* ETH DMA descriptors + ring buffers in D2 SRAM3 (0x30040000, 32KB).
   * Regione dedicata: nessun gap, nessun PT_LOAD in RAM non inizializzata. */
  .eth_dma (NOLOAD) :
  {
    *(.eth_dma)
  } >ETH_SRAM3

  /* Rimuovi metadati inutili */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
