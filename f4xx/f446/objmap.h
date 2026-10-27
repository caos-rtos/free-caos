// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/** ======================================================================= */
/** FILE WHERE DEFINITIONS FOR MCU ARE SET */
/** ======================================================================= */
#ifndef OBJMAP_H_F449
#define OBJMAP_H_F449


#undef MAX_TASK
/** TLB = 4K/180 (sizeof ca_thread_context_t) = 22 max.
 *  On this part the TLB is at the top of normal RAM, not in CCM: the F446
 *  has no CCM RAM (see the linker script). Size is unchanged. */
#define MAX_TASK   22
#define MAX_TIMER  14 
#define DUMP_BUFFER_SIZE    128

/* Number of network pool buffers (NET_BUF_SIZE = 2048 B each).
 * It lives HERE and nowhere else: see the note in caos/net/inc/net_buf.h.
 * RAM 128K like the F407 (NOT 256K like the F429, which this file was derived
 * from): with 16, the 32KB of .net_pool overflowed the RAM by ~13KB on ex55
 * and ex69-83. 8*2048 = 16KB. */
#define NET_BUF_COUNT   8

/* stdout/CLI static queues, DEFAULT_MSG_SIZE (132 B) per slot.
 * They live here and NOWHERE ELSE, like NET_BUF_COUNT. Other MCUs: 64/32.
 * With USB NCM + net (ex76) .bss left only 160 B of heap, below the minimum
 * ca_memory_init() accepts: heap never initialised, `free` silent.
 * 32 >= MAX_TASK keeps one line per task in flight; saves ~6.2 KB. */
#define STDOUT_QUEUE_DEPTH  32
#define CLI_QUEUE_DEPTH     16U



/**
 * @brief Default stack sizes for tasks for this mcu
 * @note Minimum ~240 bytes for context + frame, so STACK_SMALL is the minimum usable
 */
/* NOTE: every stack size below MUST be a multiple of 32 bytes.
 * The allocator rounds up, but the values are what keeps stack_end predictable:
 * stack_end = alloc_base + 32 has to stay 32-byte aligned because the MPU stack
 * guard region is programmed on it (32 B is the minimum MPU region granule, and
 * on ARMv7-M the guard depth is the largest power of 2 alloc_base is aligned to).
 * A value like 777 misaligns stack_end and silently weakens the guard. */
#define STACK_MIN       512
typedef enum {
    STACK_SMALL  = (STACK_MIN*2),       /**< Simple tasks (minimum usable) */
    STACK_MEDIUM = (STACK_SMALL*2),     /**< Normal tasks */
    STACK_LARGE  = (STACK_MEDIUM*2),    /**< Complex tasks (printf, etc) */
    STACK_XLARGE = (STACK_LARGE*2),     /**< Tasks with large buffers */
    STACK_KERNEL = (STACK_LARGE*2)      /**< Main task / kernel */
} ca_stack_size_t;

/* Max depth of the MPU deep stack guard (ARMv7-M only: the ARMv8-M parts
 * program an exact full-span guard and ignore this).
 * Sized on this part: arena PSP 48 KB, STACK_MEDIUM 2 KB. The real depth is the largest power-of-2
 * block alloc_base is aligned to, capped here and clamped to the arena. */
#define DEEP_GUARD_MAX  0x2000U   /* 8 KB */


/* =============================================================================
 * CLOCK FREQUENCIES (configured by clock.c)
 * ============================================================================= */
// HSE FROM CMD LINE ( CMAKE )
#ifndef HSE_VALUE
#error "Define HSE VALUE IN MAKEFILE !!!"
#endif
#define SYSCLK_FREQ_HZ      168000000UL
#define HCLK_FREQ_HZ        168000000UL
#define PCLK1_FREQ_HZ       42000000UL   /* APB1 max 42MHz */
#define PCLK2_FREQ_HZ       84000000UL   /* APB2 max 84MHz */

#define OS_TICK_US CA_TICK_SPEED_250US   /* 250 us 17/10: slices skip the re-picks (fast_tick), see 'sched' */
#define CA_SLICE_MIN_TICKS  2U           /* equal levels keep the 500 us period of before */
#define USB_EP_MAX      4U     /* EP0..3 — OTG_FS; OTG_HS=6 (future) */
#define USB_EP0_MPS     64U
#define USB_CDC_EP_DATA   1U
#define USB_CDC_EP_NOTIF  2U
#define USB_MSC_EP_DATA   3U
#define USB_NCM_EP_DATA   1U
#define USB_NCM_EP_NOTIF  2U

#define EVAL_F1XX(MSG)         
#define EVAL_F103(MSG)         

#define EVAL_F4XX(MSG)       MSG  
#define EVAL_F407(MSG)         
#define EVAL_F429(MSG)         
#define EVAL_F446(MSG)       MSG  

#define EVAL_F7XX(MSG)         
#define EVAL_F746(MSG)          

#define EVAL_H5XX(MSG)          
#define EVAL_H563(MSG)          

#define EVAL_H7XX(MSG)         
#define EVAL_H747(MSG)          
#define EVAL_H753(MSG)          

/* =============================================================================
 * HARDWARE CAPABILITIES / MPU REGIONS  (Cortex-M4, 8 MPU regions)
 *
 * MCU_HAS_ETH is POSITIVE on purpose: the old MCU_NO_ETH marked only the
 * exception, so a board that simply forgot to declare it looked like it had
 * an ETH MAC. F446: no ETH MAC (eth_f4xx.c is a stub here), OTG only
 *
 * Kernel reserves regions 0..OS_REGIONS-1 (mpu.h: canary MSP/PSP/HEAP, =5);
 * driver DMA-coherent regions start right after, one per peripheral pool.
 * ============================================================================= */
/* F446 has no ETH MAC (eth_f4xx.c guards MCU_ID==446): NET can only run over USB-NCM.
 * Kept alongside MCU_HAS_ETH because the os_config files and
 * caos.c/cli.c still test '#ifdef MCU_NO_ETH'. */
#define MCU_NO_ETH          1
#define MCU_HAS_ETH         0U   /* ETH MAC present on this part */

/* F446 has no TRNG: stm32f446xx.h does not define the RNG base, while
 * stm32f407xx.h does - it is the one peripheral that differs INSIDE the F4
 * family, which is why rng_f4xx.c carries an MCU_ID==446 stub.
 *
 * NOT mirrored by an MCU_NO_RNG that switches EN_RNG off, unlike ETH: the
 * ca_rng_* API stays compiled and rng.c falls back to a software CSPRNG, so
 * ed25519 / aes_channel / ssh keep building. This flag is what selects the
 * backend, and ca_rng_is_hardware() reports it at runtime. */
#define MCU_HAS_RNG         0U   /* TRNG present on this part */
#define MCU_HAS_QSPI        1U   /* QUADSPI/OCTOSPI1 present on this part */
#define MCU_HAS_USB         1U   /* USB device controller present */
#define MCU_HAS_GPDMA       0U   /* GPDMA (channels) instead of DMA1/DMA2 (streams) */
#define MPU_REGION_COUNT    8U  /* HW regions, kernel uses OS_REGIONS */

#define USB_MPU_REGION      3U   /* .usb_dma (USB DMA descriptors/buffers) */
#define NET_MPU_REGION      4U   /* .net_pool (zero-copy net buffer pool) */

#define ENABLE_DBG_UART    1

#if ENABLE_DBG_UART
#define UART_DBG_NUM       3
#endif
 

/* =============================================================================
 * PHYSICAL MEMORY MAP (ca_memmap_get / CLI "memmap")
 *
 * Taken from this board's linker script and cross-checked against the
 * datasheet. Physical areas only: heap, MSP, PSP and the driver pools all
 * live inside these and are deliberately NOT told apart.
 *
 * RAM areas are numbered by ASCENDING ADDRESS, so the same index means a
 * different memory on another part - knowing what each area is, is up to
 * whoever reads the listing.
 *
 * NO CCM RAM on the F446: the entry at 0x10000000 that used to be here came
 * from the F407 map and pointed at an address this part does not implement
 * (stm32f446xx.h has no CCMDATARAM_BASE). SRAM1 112 KB and SRAM2 16 KB are
 * CONTIGUOUS from 0x20000000, so they are listed as the single 128 KB block
 * the linker script actually allocates from.
 * ============================================================================= */
#define CA_MEMMAP_COUNT     2U
#define CA_MEMMAP_TABLE  \
    { 0x20000000UL, 128UL * 1024UL, "RAM" }, \
    { 0x08000000UL, 512UL * 1024UL, "FLASH" }

/* =============================================================================
 * ONBOARD LEDs (ca_led_* / CLI "led")
 *
 * NUCLEO-F446ZE, UM1974 Rev 11 section 7.5: LD1 green PB0, LD2 blue PB7,
 * LD3 red PB14. All three are active HIGH ("on when the I/O is HIGH").
 *
 * LD1 is PB0 only with SB120 ON / SB119 OFF, which is the factory default;
 * the alternate strap moves it to PA5 (Zio D13). LD2 needs SB139 ON, LD3
 * needs SB118 ON - both are ON out of the box.
 * ============================================================================= */
#define CA_LED_COUNT        3U
#define CA_LED_TABLE  \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_0,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_7,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_14, 0U }

#endif  /* OBJMAP_H_F449 */