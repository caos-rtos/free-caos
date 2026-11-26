// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/** ======================================================================= */
/** FILE WHERE DEFINITIONS ARE SET FOR MCU */
/** ======================================================================= */
#ifndef OBJMAP_H_F439
#define OBJMAP_H_F439


#undef MAX_TASK
/** CCMRAM TLB = 4K/180 (sizeof ca_thread_context_t) = 22 max */
#define MAX_TASK   22
#define MAX_TIMER  14 
#define DUMP_BUFFER_SIZE    128
/* Stack watermark alarm (idle scan), per mille of the task stack */
#define STACK_WARN_PERMILLE 750U

/* Number of network pool buffers (NET_BUF_SIZE = 2048 B each).
 * It lives HERE and nowhere else: see the note in caos/net/inc/net_buf.h.
 * RAM 192K + 64K CCM: 32*2048 = 64KB of .net_pool. 16 ran dry under SSH/TLS
 * bursts (ex45/67/80/83 30/10); 32 as on the U575, heap still ~48 KB. */
#define NET_BUF_COUNT   32

/* stdout/CLI static queues, DEFAULT_MSG_SIZE (132 B) per slot.
 * They live here and NOWHERE ELSE, like NET_BUF_COUNT. */
#define STDOUT_QUEUE_DEPTH  64
#define CLI_QUEUE_DEPTH     32U


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
#define STACK_MIN       512      /* smallest remainder the stack arena splits off */
/* STACK_SLICE: the step of the task stack sizes (21/11/2026, doc/stack_slice.md).
 * Each size is a fixed value chosen for this board, not a ratio: any value of
 * the form BASE + n * STACK_SLICE can be written, as long as it stays a multiple
 * of 32 B (above) - and, on ARMv7-M, a multiple of 2 KB, which keeps the deep
 * MPU guard at least 2 KB deep. Check: tools/stack_usage.py --tasks.
 * 2/2/4/6/8 KB: SMALL 2 KB since 20/11 (http task at 89 % of 1 KB, ex45) */
#define STACK_SLICE     2048U
typedef enum {
    STACK_SMALL  = (1U * STACK_SLICE),     /**< Simple tasks (minimum usable) */
    STACK_MEDIUM = (1U * STACK_SLICE),     /**< Normal tasks */
    STACK_LARGE  = (2U * STACK_SLICE),     /**< Complex tasks (printf, etc) */
    STACK_XLARGE = (3U * STACK_SLICE),     /**< Tasks with large buffers */
    STACK_KERNEL = (4U * STACK_SLICE)      /**< Main task / kernel */
} ca_stack_size_t;

/* Max depth of the MPU deep stack guard (ARMv7-M only: the ARMv8-M parts
 * program an exact full-span guard and ignore this).
 * Sized on this part: arena PSP 52 KB, STACK_MEDIUM 2 KB. The real depth is the largest power-of-2
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
#define EVAL_F439(MSG)       MSG  
#define EVAL_F446(MSG)         

#define EVAL_F7XX(MSG)         
#define EVAL_F746(MSG)          

#define EVAL_H5XX(MSG)          
#define EVAL_H563(MSG)          

#define EVAL_H7XX(MSG)         
#define EVAL_H747(MSG)          
#define EVAL_H753(MSG)          

#define EVAL_H7RSXX(MSG)
#define EVAL_H773(MSG)

/* =============================================================================
 * HARDWARE CAPABILITIES / MPU REGIONS  (Cortex-M4, 8 MPU regions)
 *
 * MCU_HAS_ETH is POSITIVE on purpose: the old MCU_NO_ETH marked only the
 * exception, so a board that simply forgot to declare it looked like it had
 * an ETH MAC. F439: ETH MAC + OTG FS/HS, CRYP + HASH (caOS stubs on F4)
 *
 * Kernel reserves regions 0..OS_REGIONS-1 (mpu.h: canary MSP/PSP/HEAP, =5);
 * driver DMA-coherent regions start right after, one per peripheral pool.
 * ============================================================================= */
#define MCU_HAS_ETH         1U   /* ETH MAC present on this part */
#define MCU_HAS_RNG         1U   /* TRNG present on this part */
#define MCU_HAS_QSPI        0U   /* no QUADSPI/OCTOSPI on this part */
#define MCU_HAS_USB         1U   /* USB device controller present */
#define MCU_HAS_GPDMA       0U   /* GPDMA (channels) instead of DMA1/DMA2 (streams) */
#define MPU_REGION_COUNT    8U  /* HW regions, kernel uses OS_REGIONS */

#define USB_MPU_REGION      3U   /* .usb_dma (USB DMA descriptors/buffers) */
#define NET_MPU_REGION      4U   /* .net_pool (zero-copy net buffer pool) */
#define ETH_MPU_REGION      5U   /* .eth_dma (ETH DMA descriptor rings) */

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
 * ============================================================================= */
#define CA_MEMMAP_COUNT     3U
#define CA_MEMMAP_TABLE  \
    { 0x10000000UL, 64UL * 1024UL, "RAM1" }, \
    { 0x20000000UL, 192UL * 1024UL, "RAM2" }, \
    { 0x08000000UL, 2048UL * 1024UL, "FLASH" }

/* =============================================================================
 * ONBOARD LEDs (ca_led_* / CLI "led")
 *
 * NUCLEO-F439ZI, UM1974: LD1 green PB0, LD2 blue PB7, LD3 red PB14.
 * All three are active HIGH (UM1974 7.5). Board arrived 29/10/2026, not yet run.
 * ============================================================================= */
#define CA_LED_COUNT        3U
#define CA_LED_TABLE  \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_0,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_7,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_14, 0U }

#endif  /* OBJMAP_H_F439 */