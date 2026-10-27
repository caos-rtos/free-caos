// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/** ======================================================================= */
/** FILE WHERE DEFINITIONS FOR MCU ARE SET */
/** ======================================================================= */

#ifndef OBJMAP_H_F767
#define OBJMAP_H_F767



#undef MAX_TASK
/** MAX_TASK has to satisfy TWO constraints, and the tighter one wins:
 *
 *   1) TLB   : 8 KB / 180 B (sizeof ca_thread_context_t) = 45 tasks.
 *              ca_scheduler_init() zeroes MAX_TASK * sizeof(ca_thread_context_t)
 *              at _tlb_area_base, so MAX_TASK and _tlb_area_size in
 *              STM32F767ZITX_FLASH.ld must stay in agreement: raising one
 *              without the other writes past _e_tlb, onto the bottom of the
 *              PSP arena, at every boot.
 *   2) ARENA : 116 KB / STACK_MEDIUM 4 KB = 29 tasks, if they all take a
 *              MEDIUM stack. The stacks are NOT in the TLB but in the PSP arena.
 *
 *  The first round had set 47 looking only at the TLB: 47 MEDIUM tasks want
 *  188 KB against the arena's 116, i.e. 72 KB more than there is. 25 keeps a
 *  margin on constraint 2 (29), as the F746 does at 23 of 28 possible. It is
 *  not a limit of "real" tasks: it is the number beyond which the arena no
 *  longer guarantees a stack to all of them. */
#define MAX_TASK   25
#define MAX_TIMER  32
#define DUMP_BUFFER_SIZE    128

/* Number of network pool buffers (NET_BUF_SIZE = 2048 B each).
 * It lives HERE and nowhere else: see the note in caos/net/inc/net_buf.h.
 * 64*2048 = 128KB of .net_pool in SRAM1, declared in the linker script. */
#define NET_BUF_COUNT   64

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
#define STACK_MIN       1024
typedef enum {
    STACK_SMALL  = (STACK_MIN*2),       /**< Simple tasks (minimum usable) */
    STACK_MEDIUM = (STACK_SMALL*2),     /**< Normal tasks */
    STACK_LARGE  = (STACK_MEDIUM*2),    /**< Complex tasks (printf, etc) */
    STACK_XLARGE = (STACK_LARGE*2),     /**< Tasks with large buffers */
    STACK_KERNEL = (STACK_LARGE*2)      /**< Main task / kernel */
} ca_stack_size_t;

/* Max depth of the MPU deep stack guard (ARMv7-M only: the ARMv8-M parts
 * program an exact full-span guard and ignore this).
 * Sized on this part: arena PSP 116 KB, STACK_MEDIUM 4 KB. The real depth is the largest power-of-2
 * block alloc_base is aligned to, capped here and clamped to the arena. */
#define DEEP_GUARD_MAX  0x2000U   /* 8 KB */


/* =============================================================================
 * CLOCK FREQUENCIES (configured by clock.c)
 * ============================================================================= */
// HSE FROM CMD LINE ( CMAKE )
#ifndef HSE_VALUE
#error "Define HSE VALUE IN MAKEFILE !!!"
#endif
#define SYSCLK_FREQ_HZ      216000000UL  /* Core at 216MHz (Max) */
#define HCLK_FREQ_HZ        216000000UL  /* AHB Bus at 216MHz */
#define PCLK1_FREQ_HZ       54000000UL   /* APB1 max 54MHz */
#define PCLK2_FREQ_HZ       108000000UL  /* APB2 max 108MHz */

#define OS_TICK_US CA_TICK_SPEED_200US   /* 200 us 17/10: slices skip the re-picks (fast_tick), see 'sched' */
#define CA_SLICE_MIN_TICKS  2U           /* equal levels keep the 400 us period of before */
#define USB_EP_MAX      6U     /* EP0..5 — RM0410 Table 331: OTG_FS 6 bidirectional EP */
#define USB_EP0_MPS     64U
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

#define EVAL_F7XX(MSG)         MSG
#define EVAL_F746(MSG)
#define EVAL_F767(MSG)         MSG

#define EVAL_H5XX(MSG)
#define EVAL_H563(MSG)

#define EVAL_H7XX(MSG)
#define EVAL_H747(MSG)
#define EVAL_H753(MSG)

/* =============================================================================
 * HARDWARE CAPABILITIES / MPU REGIONS  (Cortex-M7, 8 MPU regions)
 *
 * MCU_HAS_ETH is POSITIVE on purpose: the old MCU_NO_ETH marked only the
 * exception, so a board that simply forgot to declare it looked like it had
 * an ETH MAC. F767: ETH MAC + OTG FS + OTG HS
 *
 * Kernel reserves regions 0..OS_REGIONS-1 (mpu.h: canary MSP/PSP/HEAP, =5);
 * driver DMA-coherent regions start right after, one per peripheral pool.
 * ============================================================================= */
#define MCU_HAS_ETH         1U   /* ETH MAC present on this part */
#define MCU_HAS_RNG         1U   /* TRNG present on this part */
#define MCU_HAS_QSPI        1U   /* QUADSPI/OCTOSPI1 present on this part */
#define MCU_HAS_USB         1U   /* USB device controller present */
#define MCU_HAS_GPDMA       0U   /* GPDMA (channels) instead of DMA1/DMA2 (streams) */
#define MPU_REGION_COUNT    8U  /* HW regions, kernel uses OS_REGIONS */

#define USB_MPU_REGION      3U   /* .usb_dma (USB DMA descriptors/buffers) */
#define NET_MPU_REGION      4U   /* .net_pool (zero-copy net buffer pool) */
#define ETH_MPU_REGION      5U   /* .eth_dma (ETH DMA descriptor rings) */

#define ENABLE_DBG_UART    1

#if ENABLE_DBG_UART
/* UM1974 §7.9 Table 9: the ST-LINK virtual COM port is USART3 on PD8/PD9
 * (SB5/SB6 ON by default). Verified on the user manual, not assumed. */
#define UART_DBG_NUM       3
#endif


/* =============================================================================
 * PHYSICAL MEMORY MAP (ca_memmap_get / CLI "memmap")
 *
 * Taken from this board's linker script and cross-checked against the
 * datasheet (DS11532 §3.5). Physical areas only: heap, MSP, PSP and the
 * driver pools all live inside these and are deliberately NOT told apart.
 *
 * RAM areas are numbered by ASCENDING ADDRESS, so the same index means a
 * different memory on another part - knowing what each area is, is up to
 * whoever reads the listing.
 * ============================================================================= */
#define CA_MEMMAP_COUNT     4U
#define CA_MEMMAP_TABLE  \
    { 0x20000000UL, 128UL * 1024UL, "RAM1" }, \
    { 0x20020000UL, 368UL * 1024UL, "RAM2" }, \
    { 0x2007C000UL, 16UL * 1024UL, "RAM3" }, \
    { 0x08000000UL, 2048UL * 1024UL, "FLASH" }

/* =============================================================================
 * ONBOARD LEDs (ca_led_* / CLI "led")
 *
 * NUCLEO-F767ZI, UM1974 §7.5: LD1 green PB0, LD2 blue PB7, LD3 red PB14.
 * All three are active HIGH.
 *
 * ⚠️ LD2 is PB7, NOT PE1: the PE1 value in the F746 objmap is wrong (that file
 * states "NOT YET VALIDATED ON HARDWARE"). The three Nucleo-144 boards share
 * the MB1137, so the same PB7 as U575 and F446 applies here.
 * ============================================================================= */
#define CA_LED_COUNT        3U
#define CA_LED_TABLE  \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_0,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_7,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_14, 0U }

#endif  /* OBJMAP_H_F767 */
