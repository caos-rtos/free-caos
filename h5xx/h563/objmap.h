// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/** ======================================================================= */
/** FILE WHERE DEFINITIONS FOR MCU ARE SET */
/** ======================================================================= */

#ifndef OBJMAP_H_H563
#define OBJMAP_H_H563   




#undef MAX_TASK
/** ITCRAM TLB = 16K/180 (sizeof ca_thread_context_t) = 91 max; 90 with margin */
#define MAX_TASK   90
#define MAX_TIMER           32
#define DUMP_BUFFER_SIZE    128

/* Number of network pool buffers (NET_BUF_SIZE = 2048 B each).
 * It lives HERE and nowhere else: see the note in caos/net/inc/net_buf.h.
 * 64*2048 = 128KB of .net_pool, mapped by the linker in ETH_RAM. */
#define NET_BUF_COUNT   64

/* stdout/CLI static queues, DEFAULT_MSG_SIZE (132 B) per slot.
 * They live here and NOWHERE ELSE, like NET_BUF_COUNT. */
#define STDOUT_QUEUE_DEPTH  96     /* >= MAX_TASK: one line per task in flight */
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
#define STACK_MIN       2048
typedef enum {
    STACK_SMALL  = (STACK_MIN*2),       /**< Simple tasks (minimum usable) */
    STACK_MEDIUM = (STACK_SMALL*2),     /**< Normal tasks */
    STACK_LARGE  = (STACK_MEDIUM*2),    /**< Complex tasks (printf, etc) */
    STACK_XLARGE = (STACK_LARGE*2),     /**< Tasks with large buffers */
    STACK_KERNEL = (STACK_LARGE*2)      /**< Main task / kernel */
} ca_stack_size_t;



/* =============================================================================
 * CLOCK FREQUENCIES (configured by clock.c)
 * ============================================================================= */
// HSE FROM CMD LINE ( CMAKE )
#ifndef HSE_VALUE
#error "Define HSE VALUE IN MAKEFILE !!!"
#endif
#define SYSCLK_FREQ_HZ      250000000UL
#define HCLK_FREQ_HZ        250000000UL
#define PCLK1_FREQ_HZ       250000000UL  /* APB1 */
#define PCLK2_FREQ_HZ       250000000UL  /* APB2 */
#define PCLK3_FREQ_HZ       250000000UL  /* APB3 */


#define OS_TICK_US          CA_TICK_SPEED_100US   /* 100 us 17/10: slices skip the re-picks (fast_tick), see 'sched' */
#define CA_SLICE_MIN_TICKS  2U                    /* equal levels keep the 200 us period of before */
#define USB_EP_MAX      8U      /* EP0..7 — RM0481 Table 608: 8 endpoints, PMA 2048B */
#define USB_EP0_MPS     64U
#define USB_CDC_EP_DATA   3U
#define USB_CDC_EP_NOTIF  4U
#define USB_MSC_EP_DATA   2U
#define USB_NCM_EP_DATA   1U
#define USB_NCM_EP_NOTIF  5U

/* MPU region for .net_pool: on M33 (H563) net_buf.c's MPU setup is compiled
 * out (guarded by __DCACHE_PRESENT, which core_cm33.h leaves undefined —
 * DCACHE1 does not cover internal SRAM here, see reference_family_hw_quirks
 * memory note), so this value is never applied at runtime. Still required
 * by net_buf.h's #ifndef guard. */

/* SPI — NUCLEO-H563ZI (MB1404): default pins are occupied by Ethernet.
 *   UM3115 Table 16: PA7 = RMII_CRS_DV (SB38 ON), PB15 = RMII_TXD1 (JP6 ON).
 * On this board SPI1 and SPI2 must be opened with .alternate_gpio = true:
 *   SPI1 ALT: PB3=SCK (CN12-31), PB4=MISO (CN12-27), PB5=MOSI (CN12-29)  AF5
 *   SPI2 ALT: PB10=SCK, PC2=MISO, PC3=MOSI                               AF5
 * (DS14258 Rev 3 Table 15 — AF verified; UM3115 Table 21 "SPI_B")
 * NOTE PB3: it is also SWO towards ST-LINK (SB33 ON). Harmless — SWO is an input
 * and is not needed for SWD debug; remove SB33 only to use trace. */

#define EVAL_F1XX(MSG)
#define EVAL_F103(MSG)         

#define EVAL_F4XX(MSG)         
#define EVAL_F407(MSG)         
#define EVAL_F429(MSG)         
#define EVAL_F446(MSG)         

#define EVAL_F7XX(MSG)         
#define EVAL_F746(MSG)          

#define EVAL_H5XX(MSG)          MSG
#define EVAL_H563(MSG)          MSG

#define EVAL_H7XX(MSG)          
#define EVAL_H747(MSG)          
#define EVAL_H753(MSG)     

/* ex63: set to 0 (USART3 VCP used by DMA protocol) */
/* =============================================================================
 * HARDWARE CAPABILITIES / MPU REGIONS  (Cortex-M33, 16 MPU regions)
 *
 * MCU_HAS_ETH is POSITIVE on purpose: the old MCU_NO_ETH marked only the
 * exception, so a board that simply forgot to declare it looked like it had
 * an ETH MAC. H563: ETH MAC + USB_DRD_FS
 *
 * Kernel reserves regions 0..OS_REGIONS-1 (mpu.h: canary MSP/PSP/HEAP, =5);
 * driver DMA-coherent regions start right after, one per peripheral pool.
 * ============================================================================= */
#define MCU_HAS_ETH         1U   /* ETH MAC present on this part */
#define MCU_HAS_RNG         1U   /* TRNG present on this part */
#define MCU_HAS_QSPI        1U   /* QUADSPI/OCTOSPI1 present on this part */
#define MCU_HAS_USB         1U   /* USB device controller present */
#define MCU_HAS_GPDMA       1U   /* GPDMA (channels) instead of DMA1/DMA2 (streams) */
#define MPU_REGION_COUNT    16U  /* HW regions, kernel uses OS_REGIONS */

#define USB_MPU_REGION      3U   /* .usb_dma (USB DMA descriptors/buffers) */
#define NET_MPU_REGION      4U   /* .net_pool (zero-copy net buffer pool) */
#define ETH_MPU_REGION      5U   /* .eth_dma (ETH DMA descriptor rings) */
#define RO_MPU_REGION       15U  /* factory OTP/RO area, non-cacheable (ICACHE) */

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
 * RAM is 640 KB, NOT the 500 KB of the linker's MEMORY{} block. SRAM1 (256K)
 * + SRAM2 (64K) + SRAM3 (320K) are contiguous and alias at 0x20000000, ending
 * at 0x200A0000 - checked against RM0481 Figure 2 and DS14258 section 3.5.
 * MEMORY{} stops at 500 KB on purpose, so the linker allocates .data/.bss/
 * .heap only there; the top 140 KB (TLB, PSP, MSP, msp_fault) are placed by
 * hand at absolute addresses in the same script. Both are real RAM.
 *
 * Declaring only 500 KB here made ca_valid_pointer() reject every task stack,
 * since PSP lives at 0x20081000: ex6 printed "( err : invalid pointer )" for
 * a task_name that was perfectly valid (H563, 2026-08-28).
 * ============================================================================= */
/* Secret area (EN_TAMP, ca_memory_reserve_secret): SRAM2, which a confirmed
 * tamper erases in hardware and a potential tamper blocks (RM0481 table 507). It sits
 * inside the heap; reserving it keeps every other allocation out. */
#define CA_SECRET_AREA_BASE 0x20040000UL
#define CA_SECRET_AREA_SIZE (64UL * 1024UL)

#define CA_MEMMAP_COUNT     2U
#define CA_MEMMAP_TABLE  \
    { 0x20000000UL, 640UL * 1024UL, "RAM1" }, \
    { 0x08000000UL, 2048UL * 1024UL, "FLASH" }

/* =============================================================================
 * ONBOARD LEDs (ca_led_* / CLI "led")
 *
 * NUCLEO-H563ZI, UM3115: LD1 green PB0, LD2 yellow PF4, LD3 red PG4.
 * All three are active HIGH. NOT YET VALIDATED ON HARDWARE.
 *
 * NOTE: the old per-example blocks claimed this board had a single LED and
 * used PB1/PB2 as filler. It has three, and PF4/PG4 are the real ones.
 * ============================================================================= */
#define CA_LED_COUNT        3U
#define CA_LED_TABLE  \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_0,  0U }, \
    { CA_GPIO_PORT_F, CA_GPIO_PIN_4,  0U }, \
    { CA_GPIO_PORT_G, CA_GPIO_PIN_4,  0U }

#endif /* OBJMAP_H_H563 */