/* A.C. : LINKER SCRIPT FOR STM32H7S3xx (NUCLEO-H7S3L8) - image in XSPI2 flash */
/* 32 MB Octo-SPI flash (XIP) @ 0x70000000, 456 KB AXI-SRAM + 64 KB DTCM          */
/* The 64 KB internal flash holds only the trampoline (workspace/app_5), which
 * maps XSPI2 and jumps to the vector table at the start of this image. */

ENTRY(Reset_Handler)

/* === DTCM layout (64 KB = 0x10000) @ 0x20000000, option bytes as shipped ===
 * DTCM used for task stacks (as on H753, with half the room).
 *
 * 0x20010000  (top DTCM)
 * 0x2000FF00 - 0x20010000 : msp_fault (256 B)
 * 0x2000E000 - 0x2000FF00 : msp_stack (8KB - 256B)
 * 0x20002000 - 0x2000E000 : psp_stack (48 KB)
 * 0x20000000 - 0x20002000 : tlb_area  (8 KB)
 *
 * DTCM_AXI_SHARE (FLASH_OBW2SR) can grow DTCM to 128/192 KB at the cost of
 * AXI SRAM3: an option byte change, not done here.
 *
 * === AXI-SRAM (456 KB, contiguous) @ 0x24000000 ===
 * Data/BSS/Heap, then the USB and network pools (MPU-aligned to their size).
 * === AHB-SRAM1/2 (32 KB) @ 0x30000000 ===
 * ETH DMA descriptors + ring buffers.
 */

/* === Valori assoluti per evitare dipendenze circolari === */
_estack = 0x2000FF00;

/* TLB area in DTCM */
_tlb_area_base = 0x20000000;
_tlb_area_size = 0x2000;      /* 8KB */
_s_tlb = _tlb_area_base;
_e_tlb = 0x20002000;

/* PSP stacks in DTCM */
_s_psp_stack = 0x20002000;
_psp_stack_size = 0xC000;     /* 48KB */
_e_psp_stack = 0x2000E000;
_psp_stack_top = _e_psp_stack;
_psp_stack_base = _s_psp_stack;

/* MSP stack in DTCM */
_msp_stack_base = 0x2000E000;
_msp_stack_top = 0x2000FF00;
_msp_fault_base = 0x2000FF00;
_msp_fault_top = 0x20010000;

/* === Definizione memorie === */
/* AXI SRAM: SRAM1 64K (+64K shared with ITCM) 0x24000000, SRAM2 128K 0x24020000,
 *           SRAM3 128K 0x24040000, SRAM4 72K 0x24060000 (ECC off) = 456 KB
 *           (RM0477 2.4, tables 8-14)
 *   RAM       : 0x24000000, 256KB — data/bss/heap
 *   USB_SRAM  : 0x24040000, 128KB — USB DMA + queue pools (MPU region, 128K-aligned,
 *               the size H753 gives it: the USB FS queue pools alone are ~104 KB)
 *   NET_SRAM  : 0x24060000,  72KB — net buffer pool 36x2KB, all of SRAM4 (the MPU
 *               region rounds up to 256 KB at 0x24040000, same attributes as USB)
 * AHB SRAM:
 *   ETH_SRAM  : 0x30000000,  32KB — ETH DMA descriptors + ring bufs         */

/* Flash window of the image. Default: the whole XSPI2 flash. An image for a
 * slot is linked with -Wl,--defsym=__caos_flash_origin=... and __caos_flash_length. */
__caos_flash_origin = DEFINED(__caos_flash_origin) ? __caos_flash_origin : 0x70000000;
__caos_flash_length = DEFINED(__caos_flash_length) ? __caos_flash_length : 32768K;

MEMORY
{
  FLASH     (rx)  : ORIGIN = __caos_flash_origin, LENGTH = __caos_flash_length
  RAM       (rwx) : ORIGIN = 0x24000000, LENGTH = 256K
  USB_SRAM  (rw)  : ORIGIN = 0x24040000, LENGTH = 128K
  NET_SRAM  (rw)  : ORIGIN = 0x24060000, LENGTH = 72K
  ETH_SRAM  (rw)  : ORIGIN = 0x30000000, LENGTH = 32K
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

  /* Dati inizializzati. .RamFunc also holds the code that must not run from
   * XSPI2 while XSPI2 is being touched (clock changes of its kernel clock). */
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

  /* USB DMA buffers, AXI SRAM3 (0x24040000, 128KB, MPU region USB_MPU_REGION). */
  .usb_dma (NOLOAD) :
  {
    *(.usb_dma)
  } >USB_SRAM

  /* Net buffer pool in AXI SRAM4 (0x24060000, 64KB).
   * MPU region NET_MPU_REGION (see objmap.h) configured as Normal NCacheable by
   * ca_net_buf_pool_init(). */
  .net_pool (NOLOAD) :
  {
    *(.net_pool)
  } >NET_SRAM

  /* ETH DMA descriptors + ring buffers in AHB SRAM1/2 (0x30000000, 32KB). */
  .eth_dma (NOLOAD) :
  {
    *(.eth_dma)
  } >ETH_SRAM

  /* Rimuovi metadati inutili */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
