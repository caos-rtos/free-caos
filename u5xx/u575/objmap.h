// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#ifndef OBJMAP_H_U575
#define OBJMAP_H_U575

#undef MAX_TASK
/* TLB area = 16K/180 (sizeof ca_thread_context_t) = 91 max; 90 with margin */
#define MAX_TASK            90
#define MAX_TIMER           32
#define DUMP_BUFFER_SIZE    128

/* Number of network pool buffers (NET_BUF_SIZE = 2048 B each).
 * It lives HERE and nowhere else: see the note in caos/net/inc/net_buf.h.
 * 32*2048 = 64KB of .net_pool. */
#define NET_BUF_COUNT   32

/* stdout/CLI static queues, DEFAULT_MSG_SIZE (132 B) per slot.
 * They live here and NOWHERE ELSE, like NET_BUF_COUNT. */
#define STDOUT_QUEUE_DEPTH  96     /* >= MAX_TASK: one line per task in flight */
#define CLI_QUEUE_DEPTH     32U


/* NOTE: every stack size below MUST be a multiple of 32 bytes.
 * The allocator rounds up, but the values are what keeps stack_end predictable:
 * stack_end = alloc_base + 32 has to stay 32-byte aligned because the MPU stack
 * guard region is programmed on it (32 B is the minimum MPU region granule, and
 * on ARMv7-M the guard depth is the largest power of 2 alloc_base is aligned to).
 * A value like 777 misaligns stack_end and silently weakens the guard. */
#define STACK_MIN           2048
typedef enum {
    STACK_SMALL  = (STACK_MIN*2),
    STACK_MEDIUM = (STACK_SMALL*2),
    STACK_LARGE  = (STACK_MEDIUM*2),
    STACK_XLARGE = (STACK_LARGE*2),
    STACK_KERNEL = (STACK_LARGE*2)
} ca_stack_size_t;

/* U575: 160 MHz max (VOS Range 1) */
#ifndef HSE_VALUE
#error "Define HSE_VALUE in cmake!"
#endif
#define SYSCLK_FREQ_HZ      160000000UL
#define HCLK_FREQ_HZ        160000000UL
#define PCLK1_FREQ_HZ        80000000UL  /* APB1 /2 */
#define PCLK2_FREQ_HZ        80000000UL  /* APB2 /2 */
#define PCLK3_FREQ_HZ        80000000UL  /* APB3 /2 */

#define OS_TICK_US          CA_TICK_SPEED_250US   /* 250 us 17/10: slices skip the re-picks (fast_tick), see 'sched' */
#define CA_SLICE_MIN_TICKS  2U           /* equal levels keep the 500 us period of before */
#define USB_EP_MAX      6U     /* EP0..5 — OTG_FS DWC2 */
#define USB_EP0_MPS     64U
#define USB_CDC_EP_DATA   3U
#define USB_CDC_EP_NOTIF  4U
#define USB_MSC_EP_DATA   2U
#define USB_NCM_EP_DATA   1U
#define USB_NCM_EP_NOTIF  5U

/* MPU region for .net_pool: on M33 (U575) net_buf.c's MPU setup is compiled
 * out (guarded by __DCACHE_PRESENT, undefined on this core), so this value
 * is never applied at runtime. Still required by net_buf.h's #ifndef guard.
 * (U575 has no ETH — MCU_NO_ETH below — NET runs over USB-NCM only.) */

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
#define EVAL_H7XX(MSG)
#define EVAL_H747(MSG)
#define EVAL_H753(MSG)
#define EVAL_U5XX(MSG)      MSG
#define EVAL_U575(MSG)      MSG

/* U575 has no ETH peripheral */
#define MCU_NO_ETH          1

/* USART1 = PA9/PA10 (VCP ST-LINK). With the LPUART console via jumpers
 * (ex62: PA9-PA2, PA10-PA3) it MUST be set to 0: the USART1 initialized by
 * ca_os_init would drive PA9 in contention with the LPUART TX. */
/* =============================================================================
 * HARDWARE CAPABILITIES / MPU REGIONS  (Cortex-M33, 8 MPU regions)
 *
 * MCU_HAS_ETH is POSITIVE on purpose: the old MCU_NO_ETH marked only the
 * exception, so a board that simply forgot to declare it looked like it had
 * an ETH MAC. U575: no ETH MAC, net runs over USB-NCM
 *
 * Kernel reserves regions 0..OS_REGIONS-1 (mpu.h: canary MSP/PSP/HEAP, =5);
 * driver DMA-coherent regions start right after, one per peripheral pool.
 * ============================================================================= */
#define MCU_HAS_ETH         0U   /* ETH MAC present on this part */
#define MCU_HAS_RNG         1U   /* TRNG present on this part */
#define MCU_HAS_QSPI        1U   /* QUADSPI/OCTOSPI1 present on this part */
#define MCU_HAS_USB         1U   /* USB device controller present */
#define MCU_HAS_GPDMA       1U   /* GPDMA (channels) instead of DMA1/DMA2 (streams) */
#define MPU_REGION_COUNT    8U  /* HW regions, kernel uses OS_REGIONS */

#define USB_MPU_REGION      3U   /* .usb_dma (USB DMA descriptors/buffers) */
#define NET_MPU_REGION      4U   /* .net_pool (zero-copy net buffer pool) */
#define RO_MPU_REGION       7U   /* factory OTP/engineering area, non-cacheable (ICACHE) */

#define ENABLE_DBG_UART    1

#if ENABLE_DBG_UART
#define UART_DBG_NUM       1
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
 * RAM is 768 KB, NOT the 628 KB of the linker's MEMORY{} block. SRAM1 (192K)
 * + SRAM2 (64K) + SRAM3 (512K) are contiguous from 0x20000000 and end at
 * 0x200C0000 - checked against RM0456 Figure 4 and DS13737 section 3.5.
 * MEMORY{} stops at 628 KB on purpose, so the linker allocates .data/.bss/
 * .heap only there; the top 140 KB (TLB, PSP, MSP, msp_fault) are placed by
 * hand at absolute addresses in the same script. Both are real RAM.
 * SRAM4 (16K, 0x28000000) is NOT listed: nothing in this port uses it.
 *
 * Declaring only 628 KB made ca_valid_pointer() reject every task stack,
 * since PSP ends at 0x200BE000. Same defect found and fixed on H563 first,
 * where ex6 printed "( err : invalid pointer )" for a valid task_name.
 * ============================================================================= */
/* Secret area (EN_TAMP, ca_memory_reserve_secret): SRAM2, which a confirmed
 * tamper erases in hardware and a potential tamper blocks (RM0456 table 644). It sits
 * inside the heap; reserving it keeps every other allocation out. */
#define CA_SECRET_AREA_BASE 0x20030000UL
#define CA_SECRET_AREA_SIZE (64UL * 1024UL)

#define CA_MEMMAP_COUNT     2U
#define CA_MEMMAP_TABLE  \
    { 0x20000000UL, 768UL * 1024UL, "RAM1" }, \
    { 0x08000000UL, 2048UL * 1024UL, "FLASH" }

/* =============================================================================
 * ONBOARD LEDs (ca_led_* / CLI "led")
 *
 * NUCLEO-U575ZI-Q, UM2861: LD1 green PC7, LD2 blue PB7, LD3 red PG2.
 * All three are active HIGH. VALIDATED ON HARDWARE.
 *
 * Only LEDs that physically exist are listed: ca_led_on(4) fails here instead
 * of driving some unrelated pin.
 * ============================================================================= */
#define CA_LED_COUNT        3U
#define CA_LED_TABLE  \
    { CA_GPIO_PORT_C, CA_GPIO_PIN_7,  0U }, \
    { CA_GPIO_PORT_B, CA_GPIO_PIN_7,  0U }, \
    { CA_GPIO_PORT_G, CA_GPIO_PIN_2,  0U }

#endif /* OBJMAP_H_U575 */
