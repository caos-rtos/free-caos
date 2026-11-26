// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/**
 * @file    objmap.h
 * @brief   Object mapping and clock frequencies for STM32H7S3 (NUCLEO-H7S3L8, MB1737)
 */

#ifndef OBJMAP_H_H773
#define OBJMAP_H_H773

/* =============================================================================
 * CLOCK FREQUENCIES (configured by clock_h7rsxx.c, VOS high)
 * DS14359 Table 26: CPU 600 MHz (with ECC_ON_SRAM off), AXI/AHB 300, APB 150.
 * ============================================================================= */
// HSE FROM CMD LINE ( CMAKE ): X1 crystal 24 MHz on this board (UM3276 7.7.2)
#ifndef HSE_VALUE
#error "Define HSE VALUE IN MAKEFILE !!!"
#endif
#define SYSCLK_FREQ_HZ      600000000UL     /* 600 MHz (VOS high) */
#define HCLK_FREQ_HZ        300000000UL     /* 300 MHz (AXI/AHB) */
#define PCLK1_FREQ_HZ       150000000UL     /* 150 MHz (APB1) */
#define PCLK2_FREQ_HZ       150000000UL     /* 150 MHz (APB2) */
#define PCLK4_FREQ_HZ       150000000UL     /* 150 MHz (APB4) */
#define PCLK5_FREQ_HZ       150000000UL     /* 150 MHz (APB5) */

/* =============================================================================
 * RTOS CONFIGURATION
 * ============================================================================= */

#undef MAX_TASK
/** DTCM TLB = 8K/180 (sizeof ca_thread_context_t) = 45 slots, but the real
 *  limit is the 48 KB PSP arena: 24 tasks of STACK_SMALL (2 KB). Was 44 from
 *  the TLB alone: ex91 could not create its 6th actor (01/11/2026). */
#define MAX_TASK   24
#define MAX_TIMER           32
#define DUMP_BUFFER_SIZE    128
/* Stack watermark alarm (idle scan), per mille of the task stack */
#define STACK_WARN_PERMILLE 750U

/* Number of network pool buffers (NET_BUF_SIZE = 2048 B each).
 * It lives HERE and nowhere else: see the note in caos/net/inc/net_buf.h.
 * 36*2048 = 72KB of .net_pool: all of AXI SRAM4 (0x24060000, ECC off); the
 * AHB SRAM of this part is only 32 KB. Was 32: the ETH driver keeps up to 24
 * (8 RX + 16 TX deferred-free), and 8 free against the TCP reserve of 4
 * closed every window - ex45/ex85 FAIL on the bench (01/11/2026). */
#define NET_BUF_COUNT   36

/* stdout/CLI static queues, DEFAULT_MSG_SIZE (132 B) per slot.
 * They live here and NOWHERE ELSE, like NET_BUF_COUNT. */
#define STDOUT_QUEUE_DEPTH  32     /* >= MAX_TASK: one line per task in flight */
#define CLI_QUEUE_DEPTH     32U


/**
 * @brief Default stack sizes for tasks
 * @note H7S3 has 64KB DTCM (option bytes default): 48KB of PSP arena, so the
 *       F767 scale (STACK_MIN 1024: SMALL 2K, MEDIUM 4K, LARGE 8K), not the
 *       H753 one (2048 over a 104 KB arena). DTCM_AXI_SHARE can grow DTCM to
 *       128/192 KB (option byte) if more stack is ever needed.
 */
/* NOTE: every stack size below MUST be a multiple of 32 bytes.
 * The allocator rounds up, but the values are what keeps stack_end predictable:
 * stack_end = alloc_base + 32 has to stay 32-byte aligned because the MPU stack
 * guard region is programmed on it (32 B is the minimum MPU region granule, and
 * on ARMv7-M the guard depth is the largest power of 2 alloc_base is aligned to).
 * A value like 777 misaligns stack_end and silently weakens the guard. */
#define STACK_MIN       1024      /* smallest remainder the stack arena splits off */
/* STACK_SLICE: the step of the task stack sizes (21/11/2026, doc/stack_slice.md).
 * Each size is a fixed value chosen for this board, not a ratio: any value of
 * the form BASE + n * STACK_SLICE can be written, as long as it stays a multiple
 * of 32 B (above) - and, on ARMv7-M, a multiple of 2 KB, which keeps the deep
 * MPU guard at least 2 KB deep. Check: tools/stack_usage.py --tasks.
 * 2/4/6/8/10 KB (was 2/4/8/16/16): PSP arena 48 KB */
#define STACK_SLICE     2048U
typedef enum {
    STACK_SMALL  = (1U * STACK_SLICE),     /**< Simple tasks (minimum usable) */
    STACK_MEDIUM = (2U * STACK_SLICE),     /**< Normal tasks */
    STACK_LARGE  = (3U * STACK_SLICE),     /**< Complex tasks (printf, etc) */
    STACK_XLARGE = (4U * STACK_SLICE),     /**< Tasks with large buffers */
    STACK_KERNEL = (5U * STACK_SLICE)      /**< Main task / kernel */
} ca_stack_size_t;

/* Max depth of the MPU deep stack guard (ARMv7-M only: the ARMv8-M parts
 * program an exact full-span guard and ignore this).
 * Sized on this part: arena PSP 48 KB, STACK_MEDIUM 4 KB. The real depth is the largest power-of-2
 * block alloc_base is aligned to, capped here and clamped to the arena. */
#define DEEP_GUARD_MAX  0x2000U   /* 8 KB */



#define OS_TICK_US              CA_TICK_SPEED_100US   /* as H753 */
#define CA_SLICE_MIN_TICKS  2U


#define USB_EP_MAX      9U      /* EP0..8 — OTG_HS */
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
#define EVAL_F439(MSG)
#define EVAL_F446(MSG)

#define EVAL_F7XX(MSG)
#define EVAL_F746(MSG)

#define EVAL_H5XX(MSG)
#define EVAL_H563(MSG)

#define EVAL_H7XX(MSG)
#define EVAL_H747(MSG)
#define EVAL_H753(MSG)

#define EVAL_H7RSXX(MSG)        MSG
#define EVAL_H773(MSG)          MSG

/* =============================================================================
 * HARDWARE CAPABILITIES / MPU REGIONS  (Cortex-M7, 16 MPU regions)
 *
 * H7S3: ETH MAC (RMII, LAN8742A), OTG_HS with integrated HS PHY (the only USB
 * on the board connector), XSPI1/2 instead of QUADSPI, GPDMA1/HPDMA1 instead
 * of DMA1/DMA2/MDMA/BDMA.
 *
 * Kernel reserves regions 0..OS_REGIONS-1 (mpu.h: canary MSP/PSP/HEAP, =5);
 * driver DMA-coherent regions start right after, one per peripheral pool.
 * ============================================================================= */
#define MCU_HAS_ETH         1U   /* ETH MAC present on this part */
#define MCU_HAS_RNG         1U   /* TRNG present on this part */
#define MCU_HAS_QSPI        1U   /* XSPI present on this part */
#define MCU_HAS_USB         1U   /* USB device controller present */
#define MCU_HAS_GPDMA       1U   /* GPDMA (channels) instead of DMA1/DMA2 (streams) */
#define MPU_REGION_COUNT    16U  /* HW regions, kernel uses OS_REGIONS */

#define USB_MPU_REGION      3U   /* .usb_dma (USB DMA descriptors/buffers) */
#define NET_MPU_REGION      4U   /* .net_pool (zero-copy net buffer pool) */
#define ETH_MPU_REGION      5U   /* .eth_dma (ETH DMA descriptor rings) */

#define ENABLE_DBG_UART    1

#if ENABLE_DBG_UART
#define UART_DBG_NUM       3     /* USART3 PD8/PD9 = ST-LINK VCP (UM3276 7.9) */
#endif




/* =============================================================================
 * PHYSICAL MEMORY MAP (ca_memmap_get / CLI "memmap")
 *
 * Taken from this board's linker script (option bytes as shipped: ITCM 64 KB,
 * DTCM 64 KB, ECC_ON_SRAM off -> AXI SRAM 456 KB contiguous, RM0477 2.4).
 * RAM areas by ASCENDING ADDRESS. The image runs from the external Octo-SPI
 * flash on XSPI2 (0x70000000, memory-mapped by the workspace/app_5 trampoline
 * in the 64 KB internal flash).
 * ============================================================================= */
#define CA_MEMMAP_COUNT     5U
#define CA_MEMMAP_TABLE  \
    { 0x20000000UL, 64UL * 1024UL, "DTCM" }, \
    { 0x24000000UL, 456UL * 1024UL, "RAM1" }, \
    { 0x30000000UL, 32UL * 1024UL, "RAM2" }, \
    { 0x08000000UL, 64UL * 1024UL, "IFLASH" }, \
    { 0x70000000UL, 32768UL * 1024UL, "FLASH" }

/* =============================================================================
 * ONBOARD LEDs (ca_led_* / CLI "led")
 *
 * NUCLEO-H7S3L8, UM3276 7.5: LD1 green PD10, LD2 yellow PD13, LD3 red PB7.
 * All three are active HIGH. NOT YET VALIDATED ON HARDWARE.
 * ============================================================================= */
#define CA_LED_COUNT        3U
#define CA_LED_TABLE  \
    { CA_GPIO_PORT_D, CA_GPIO_PIN_10, 0U }, \
    { CA_GPIO_PORT_D, CA_GPIO_PIN_13, 0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_7,  0U }

#endif /* OBJMAP_H_H773 */
