// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/**
 * @file    objmap.h
 * @brief   Object mapping and clock frequencies for STM32H753
 */

#ifndef OBJMAP_H_H753
#define OBJMAP_H_H753

/* =============================================================================
 * CLOCK FREQUENCIES (configured by clock.c for 480MHz @ VOS0)
 * ============================================================================= */
// HSE FROM CMD LINE ( CMAKE )
#ifndef HSE_VALUE
#error "Define HSE VALUE IN MAKEFILE !!!"
#endif
#define SYSCLK_FREQ_HZ      480000000UL     /* 480 MHz (VOS0) */
#define HCLK_FREQ_HZ        240000000UL     /* 240 MHz (AXI/AHB) */
#define PCLK1_FREQ_HZ       120000000UL     /* 120 MHz (APB1) */
#define PCLK2_FREQ_HZ       120000000UL     /* 120 MHz (APB2) */
#define PCLK3_FREQ_HZ       120000000UL     /* 120 MHz (APB3) */
#define PCLK4_FREQ_HZ       120000000UL     /* 120 MHz (APB4) */

/* =============================================================================
 * RTOS CONFIGURATION
 * ============================================================================= */

#undef MAX_TASK
/** ITCRAM TLB = 16K/180 (sizeof ca_thread_context_t) = 91 max; 90 with margin */
#define MAX_TASK   90
#define MAX_TIMER           32
#define DUMP_BUFFER_SIZE    128

/* Number of network pool buffers (NET_BUF_SIZE = 2048 B each).
 * It lives HERE and nowhere else: see the note in caos/net/inc/net_buf.h.
 * 64*2048 = 128KB of .net_pool, mapped by the linker in D2 SRAM1 (0x30000000). */
#define NET_BUF_COUNT   64

/* stdout/CLI static queues, DEFAULT_MSG_SIZE (132 B) per slot.
 * They live here and NOWHERE ELSE, like NET_BUF_COUNT. */
#define STDOUT_QUEUE_DEPTH  96     /* >= MAX_TASK: one line per task in flight */
#define CLI_QUEUE_DEPTH     32U


/**
 * @brief Default stack sizes for tasks
 * @note H753 has 128KB DTCM for stacks - generous sizes
 */
/* NOTE: every stack size below MUST be a multiple of 32 bytes.
 * The allocator rounds up, but the values are what keeps stack_end predictable:
 * stack_end = alloc_base + 32 has to stay 32-byte aligned because the MPU stack
 * guard region is programmed on it (32 B is the minimum MPU region granule, and
 * on ARMv7-M the guard depth is the largest power of 2 alloc_base is aligned to).
 * A value like 777 misaligns stack_end and silently weakens the guard. */
#define STACK_MIN       2048
typedef enum {
    STACK_SMALL  = (STACK_MIN*2),       /**< Simple tasks (minimum usable) */
    STACK_MEDIUM = (STACK_SMALL*2),     /**< Normal tasks */
    STACK_LARGE  = (STACK_MEDIUM*2),    /**< Complex tasks (printf, etc) */
    STACK_XLARGE = (STACK_LARGE*2),     /**< Tasks with large buffers */
    STACK_KERNEL = (STACK_LARGE*2)      /**< Main task / kernel */
} ca_stack_size_t;

/* Max depth of the MPU deep stack guard (ARMv7-M only: the ARMv8-M parts
 * program an exact full-span guard and ignore this).
 * Sized on this part: arena PSP 104 KB, STACK_MEDIUM 8 KB. The real depth is the largest power-of-2
 * block alloc_base is aligned to, capped here and clamped to the arena. */
#define DEEP_GUARD_MAX  0x8000U   /* 32 KB */



#define OS_TICK_US              CA_TICK_SPEED_100US   /* 100 us 17/10: slices skip the re-picks (fast_tick), see 'sched' */
#define CA_SLICE_MIN_TICKS  2U           /* equal levels keep the 200 us period of before */
                                /* at 100 us: scheduler ~14.5% of the CPU in Release, 7.5% at 200 */


#define USB_EP_MAX      9U      /* EP0..8 — RM0433: OTG_FS=6, OTG_HS=9 */
#define USB_EP0_MPS     64U
#define USB_EP0_TX_BUF   512U   /* staging EP0 TX (full composite descriptor) */
#define USB_EP0_XFR_MAX  127U   /* max XFRSIZ for single EP0 transfer (7 bit) */
#define USB_CDC_EP_DATA   3U
#define USB_CDC_EP_NOTIF  4U
#define USB_MSC_EP_DATA   2U
#define USB_NCM_EP_DATA   1U
#define USB_NCM_EP_NOTIF  5U

#define EVAL_F1XX(MSG)
#define EVAL_F103(MSG)         

#define EVAL_F4XX(MSG)         
#define EVAL_F407(MSG)         
#define EVAL_F429(MSG)         
#define EVAL_F446(MSG)         

#define EVAL_F7XX(MSG)         
#define EVAL_F746(MSG)          

#define EVAL_H5XX(MSG)          
#define EVAL_H563(MSG)          

#define EVAL_H7XX(MSG)          MSG
#define EVAL_H747(MSG)          
#define EVAL_H753(MSG)          MSG

/* =============================================================================
 * HARDWARE CAPABILITIES / MPU REGIONS  (Cortex-M7, 16 MPU regions)
 *
 * MCU_HAS_ETH is POSITIVE on purpose: the old MCU_NO_ETH marked only the
 * exception, so a board that simply forgot to declare it looked like it had
 * an ETH MAC. H753: ETH MAC + OTG FS/HS
 *
 * Kernel reserves regions 0..OS_REGIONS-1 (mpu.h: canary MSP/PSP/HEAP, =5);
 * driver DMA-coherent regions start right after, one per peripheral pool.
 * ============================================================================= */
#define MCU_HAS_ETH         1U   /* ETH MAC present on this part */
#define MCU_HAS_RNG         1U   /* TRNG present on this part */
#define MCU_HAS_QSPI        1U   /* QUADSPI/OCTOSPI1 present on this part */
#define MCU_HAS_USB         1U   /* USB device controller present */
#define MCU_HAS_GPDMA       0U   /* GPDMA (channels) instead of DMA1/DMA2 (streams) */
#define MPU_REGION_COUNT    16U  /* HW regions, kernel uses OS_REGIONS */

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
 *
 * DTCM (128K at 0x20000000, RM0433 Table 6) comes FIRST because it is the
 * lowest address, and it is where the linker puts TLB, PSP, MSP and
 * msp_fault - at absolute addresses, outside its MEMORY{} block, which only
 * covers 0x24000000 upwards. It was missing entirely until 2026-08-28, so
 * ca_valid_pointer() rejected every task stack: any %s given a pointer into
 * a task stack printed "( err : invalid pointer )". Found on H563, where the
 * same defect took the shape of a map that was merely too short.
 * ============================================================================= */
#define CA_MEMMAP_COUNT     6U
#define CA_MEMMAP_TABLE  \
    { 0x20000000UL, 128UL * 1024UL, "DTCM" }, \
    { 0x24000000UL, 512UL * 1024UL, "RAM1" }, \
    { 0x30000000UL, 128UL * 1024UL, "RAM2" }, \
    { 0x30020000UL, 128UL * 1024UL, "RAM3" }, \
    { 0x30040000UL, 32UL * 1024UL, "RAM4" }, \
    { 0x08000000UL, 2048UL * 1024UL, "FLASH" }

/* =============================================================================
 * ONBOARD LEDs (ca_led_* / CLI "led")
 *
 * NUCLEO-H753ZI, UM1974: LD1 green PB0, LD2 blue PE1, LD3 red PB14.
 * All three are active HIGH. VALIDATED ON HARDWARE.
 * ============================================================================= */
#define CA_LED_COUNT        3U
#define CA_LED_TABLE  \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_0,  0U }, \
    { CA_GPIO_PORT_E, CA_GPIO_PIN_1,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_14, 0U }

#endif /* OBJMAP_H_H753 */
