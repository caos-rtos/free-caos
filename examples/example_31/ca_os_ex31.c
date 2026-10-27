// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 31 — DMA: M2M + M2P (DAC1)
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI) and NUCLEO-U575ZI-Q (STM32U575ZI)
 *
 * Pins used:
 *   Heartbeat LED:  PB14 on the H753 (LD3 red), PG2 on the U575 (LD3 red).
 *                   The two boards share no LED pin: see the LED block below.
 *   PA4:            DAC1_OUT1 (analog) — sine wave ~1.6 Hz, same pin on both
 *
 * Test 1 — DMA M2M:
 *   Copy 256 words (1024 bytes) from m2m_src to m2m_dst every second.
 *   Verify byte by byte. Log ok/errors.
 *   No cache maintenance: see the D-cache note below.
 *
 * Test 2 — DMA M2P (DAC1):
 *   Sine table 64 samples 12-bit (0-4095).
 *   DMA copies 1 word at a time (NDTR=1) from sine_table[i] to DAC1->DHR12R1.
 *   DAC1 CH1 in continuous mode (no trigger) → immediate output on PA4.
 *   Sine period: 64 samples × 10 ms = 640 ms → ~1.56 Hz.
 *   This is a DMA test: the DAC itself is verified by example 27 (DAC->ADC sweep).
 *
 * H7xx notes:
 *   - DMA1/DMA2 use M2M mode (DIR=10) -> no DMAMUX to configure
 *   - DMA buffers must be outside TCM (ITCM/DTCM)
 *   - static globals -> AXI SRAM (0x24000000) -> DMA-safe
 *
 * F7xx notes:
 *   - DMA1/DMA2 as on the H7: M2M is DMA2 only, hence ca_dma_free_channel_m2m()
 *   - DMA buffers must be outside TCM: the DTCM at 0x20000000 is reachable by
 *     the DMAs only through a dedicated AHB slave, the ITCM not at all
 *   - static globals -> SRAM1 (F767: 0x20020000, F746: 0x20010000) -> DMA-safe.
 *     That is where the linker script puts .data/.bss, so nothing to do here
 *
 * U5xx notes:
 *   - GPDMA1, no D-cache on the Cortex-M33 at all
 *   - static globals -> SRAM1 (0x20000000) -> DMA-safe
 *
 * D-cache:
 *   caOS runs EVERY Cortex-M7 with the D-cache DISABLED - H7 and F7 alike -
 *   and the M33 has none. So this example does no cache maintenance: none is
 *   needed, and asking for it would be the defect, not the fix. Calling
 *   SCB_CleanDCache_by_Addr with the cache off faults on the M7 (an imprecise
 *   write to SCB->DCCMVAC), which is exactly the bus fault that was removed
 *   from eth_h7xx.c and sdio_h7xx.c.
 *
 *   That uniformity is recent and was NOT free. Until 2026-09-09 clock_f7xx.c
 *   enabled both caches at step "9. L1 Cache" - not a decision, but bring-up
 *   drift: the same step does not exist in clock_h7xx.c, and the F746 it was
 *   written for had no board to expose it. On the first real F7 hardware
 *   (NUCLEO-F767ZI) this example reported ERRORS=256 every second - every
 *   single word - while the DMA registers said the transfer had COMPLETED:
 *   NDTR=0, no TEIF/FEIF/DMEIF. The DMA was writing SRAM1 while the CPU read
 *   m2m_dst back through the cache, still holding the zeros written just
 *   before. It looks like a broken DMA and is not.
 *
 *   The I-cache stays ON: the DMA does not write code, so it has no such
 *   problem, and it is where most of the speed-up is.
 *
 *   If the D-cache is ever enabled on an M7, the maintenance has to come back
 *   here together with it - clean m2m_src before the transfer, invalidate
 *   m2m_dst after - guarded by SCB->CCR bit DC. __DCACHE_PRESENT is the wrong
 *   guard: it says the core HAS a cache, not that it is ON.
 *
 * REQUIREMENTS os_config.h:
 *   EN_DMA  1
 *   EN_DAC  1
 */

#include "caos.h"

#if EN_DMA && EN_DAC

#include "dma.h"
#include "dac.h"

/* ============================================================================
 * HARDWARE
 * ============================================================================ */

#if MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549, UM2861 6.4): LD1 PC7, LD2 PB7, LD3 PG2.
 * PB14 is not a LED on this board - falling through to the H753 block left
 * the heartbeat invisible while the console kept saying everything was fine. */
#else
/* NUCLEO-H753ZI (MB1364, UM2407 6.5): LD1 PB0, LD2 PE1, LD3 PB14 */
#endif

/* ============================================================================
 * DMA BUFFER — aligned to 32 bytes (Cortex-M7 cache line granularity)
 * ============================================================================ */

#define BUF_WORDS    256U
#define CACHE_LINE   32U

__attribute__((aligned(CACHE_LINE))) static u32 m2m_src[BUF_WORDS];
__attribute__((aligned(CACHE_LINE))) static u32 m2m_dst[BUF_WORDS];

/* 'e31 stuck' buffers: the M2M task keeps using the two above */
#define STUCK_WORDS  64U
__attribute__((aligned(CACHE_LINE))) static u32 stuck_src[STUCK_WORDS];
__attribute__((aligned(CACHE_LINE))) static u32 stuck_dst[STUCK_WORDS];

/* Sine table 64 samples, 12-bit (0-4095), in SRAM (not Flash) for DMA */
__attribute__((aligned(CACHE_LINE))) static u32 sine_table[64] = {
    2048, 2248, 2447, 2642, 2831, 3013, 3185, 3346,
    3495, 3630, 3749, 3851, 3934, 3999, 4044, 4069,
    4076, 4069, 4044, 3999, 3934, 3851, 3749, 3630,
    3495, 3346, 3185, 3013, 2831, 2642, 2447, 2248,
    2048, 1847, 1648, 1453, 1264, 1082,  910,  749,
     600,  465,  346,  244,  161,   96,   51,   26,
      19,   26,   51,   96,  161,  244,  346,  465,
     600,  749,  910, 1082, 1264, 1453, 1648, 1847,
};
#define SINE_SAMPLES  64U

/* ============================================================================
 * SHARED STATE
 * ============================================================================ */

static volatile u32 m2m_ok_count  = 0U;
static volatile u32 m2m_err_count = 0U;

/* ============================================================================
 * TASK: DMA M2M — copy + verify every second
 * ============================================================================ */

static u32 task_dma_m2m(void *arg)
{
    (void)arg;

    ca_dma_ch_t ch;
    /* M2M needs a controller that can do memory-to-memory: on F4/F7 that is
     * DMA2 only, and ca_dma_free_channel() would hand out DMA1_s0 - the
     * transfer never starts and ca_dma_wait() blocks forever. */
    i16 idx = ca_dma_free_channel_m2m(&ch);
    if (idx < 0) {
        ca_stdout_printf_n("EX31 M2M: no free M2M-capable DMA channel");
        return 0;
    }
    ca_stdout_printf_n("EX31 M2M: channel flat=%d (ctrl=%u str=%u)",
                       (int)idx, (unsigned)ch.controller, (unsigned)ch.stream);

    /* Fixed pattern in src */
    for (u32 i = 0U; i < BUF_WORDS; i++)
        m2m_src[i] = (i * 0x55AA0011U) ^ 0xDEADBEEFU;

    while (1) {
        /* Clear dst */
        for (u32 i = 0U; i < BUF_WORDS; i++)
            m2m_dst[i] = 0U;

        /* No cache maintenance: caOS keeps the D-cache off on every M7 and the
         * M33 has none (see the D-cache note at the top of the file). __DSB()
         * is enough to retire the buffer writes before the DMA reads them. */
        __DSB();

        ca_dma_copy_inc(ch, m2m_src, m2m_dst, BUF_WORDS, CA_DMA_WIDTH_32);
        if (!ca_dma_wait(ch, 100U))   /* block task, yield to scheduler */
            ca_stdout_put_n("[ex31] M2M: no DMA TC within 100 ms");

        /* Verify */
        u32 errs = 0U;
        for (u32 i = 0U; i < BUF_WORDS; i++) {
            if (m2m_dst[i] != m2m_src[i])
                errs++;
        }

        if (errs == 0U) {
            m2m_ok_count++;
            ca_stdout_printf_n("EX31 M2M: ok #%u (1024B)", (unsigned)m2m_ok_count);
        } else {
            m2m_err_count++;
            ca_stdout_printf_n("EX31 M2M: ERRORS=%u  #%u",
                               (unsigned)errs, (unsigned)m2m_err_count);
        }

        ca_os_sleep(1000);
    }
    return 0;
}

/* ============================================================================
 * TASK: DMA M2P → DAC1_OUT1 (PA4) — sine wave
 * ============================================================================ */

static u32 task_dma_dac(void *arg)
{
    (void)arg;

    ca_dma_ch_t ch;
    /* Also M2M: ca_dma_copy_inc() always programs DIR=memory-to-memory, even
     * when the destination is a peripheral register like DHR12R1 - so this
     * task needs an M2M-capable controller too (DMA2 on F4/F7). */
    i16 idx = ca_dma_free_channel_m2m(&ch);
    if (idx < 0) {
        ca_stdout_printf_n("EX31 DAC: no free M2M-capable DMA channel");
        return 0;
    }
    ca_stdout_printf_n("EX31 DAC: channel flat=%d -> PA4 DAC1_OUT1 sine 1.56Hz",
                       (int)idx);

    /* sine_table is in SRAM and read-only from here on; no cache maintenance
     * (D-cache off, see the note at the top of the file). */
    __DSB();

    u32 si    = 0U;
    u32 waves = 0U;

    while (1) {
        /*
         * DMA M2M trick with NDTR=1:
         *   src = &sine_table[si]  (SRAM)
         *   dst = &DAC1->DHR12R1   (peripheral, treated as a memory address;
         *                           APB1 on the H753, AHB3 on the U575, but
         *                           DHR12R1 sits at offset 0x08 on both)
         *   NDTR=1 → 1 word transferred; PINC irrelevant with NDTR=1
         *   DAC CH1 without trigger → immediate output after DHR12R1 write
         */
        ca_dma_copy_inc(ch,
                        (const void *)&sine_table[si],
                        (void *)&DAC1->DHR12R1,
                        1U, CA_DMA_WIDTH_32);
        if (!ca_dma_wait(ch, 100U))
            ca_stdout_put_n("[ex31] DAC: no DMA TC within 100 ms");

        si++;
        if (si >= SINE_SAMPLES) {
            si = 0U;
            waves++;
            ca_stdout_printf_n("EX31 DAC: wave #%u done", (unsigned)waves);
        }
        ca_os_sleep(10);   /* 10 ms/sample → 640 ms period → ~1.56 Hz */
    }
    return 0;
}

/* ============================================================================
 * TASK: heartbeat
 * ============================================================================ */

static u32 task_heartbeat(void *arg)
{
    (void)arg;
    u32 t = 0U;
    while (1) {
        ca_led_toggle(1U);
        ca_os_sleep(500);
        t++;
        if ((t % 20U) == 0U)
            ca_stdout_printf_n("EX31: uptime=%us  m2m_ok=%u  m2m_err=%u",
                               (unsigned)(t / 2U),
                               (unsigned)m2m_ok_count,
                               (unsigned)m2m_err_count);
    }
    return 0;
}

/* ============================================================================
 * caOS hooks
 * ============================================================================ */

/* ============================================================================
 * CLI "e31 stuck": a DMA transfer that never starts, given up and recovered
 *
 * On F4/F7 only DMA2 does memory-to-memory: an M2M armed on DMA1 never moves.
 * ca_dma_wait() must give up on its deadline (it blocked for good before
 * 19/10), ca_dma_abort() must stop the stream, and the copy must then work on
 * a DMA2 channel. Where every channel does M2M there is nothing to provoke.
 * ============================================================================ */
#if ENABLE_CLI
static bool ex31_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e31       : stuck  (M2M on a channel that cannot run it)");
        return false;
    }
    if (ca_strncmp(p, "e31", 3) != 0) return false;
    p = ca_skip_sp(p + 3);
    if (ca_strncmp(p, "stuck", 5) != 0) { CLI_PUT_N("e31: stuck"); return true; }

    if (ca_dma_hw_m2m_first_flat() == 0U) {
        CLI_PUT_N("E31 STUCK: not applicable - every DMA channel does M2M here");
        return true;
    }
    ca_dma_ch_t bad, good;
    if (ca_dma_free_channel_index(0, &bad) < 0) {
        CLI_PUT_N("E31 STUCK: DMA1 channel 0 is busy");
        return true;
    }
    for (u32 i = 0U; i < STUCK_WORDS; i++) { stuck_src[i] = 0xA5000000U | i; stuck_dst[i] = 0U; }
    __DSB();
    u32 t0 = ca_os_tick_ms();
    ca_dma_copy_inc(bad, stuck_src, stuck_dst, STUCK_WORDS, CA_DMA_WIDTH_32);
    const bool bad_done = ca_dma_wait(bad, 100U);
    const u32 bad_ms = ca_os_tick_ms() - t0;
    ca_dma_abort(bad);
    ca_dma_release(bad);

    bool good_ok = false;
    if (ca_dma_free_channel_m2m(&good) >= 0) {
        ca_dma_copy_inc(good, stuck_src, stuck_dst, STUCK_WORDS, CA_DMA_WIDTH_32);
        good_ok = ca_dma_wait(good, 100U);
        for (u32 i = 0U; i < STUCK_WORDS && good_ok; i++)
            if (stuck_dst[i] != stuck_src[i]) good_ok = false;
        ca_dma_release(good);
    }
    CLI_PRINTF_N("E31 STUCK: DMA1 M2M %s, wait gave up in %u ms; DMA2 copy after it %s",
                 bad_done ? "COMPLETED (unexpected)" : "never ran", bad_ms,
                 good_ok ? "OK" : "FAIL");
    return true;
}

static ca_cli_node_t s_ex31_cli_node = { ex31_cli_handler, NULL };
#endif

void ca_os_configure(void)
{
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_ex31_cli_node);
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    ca_led_init();
    ca_led_off(1U);

    ca_dma_init();
    ca_dac_init(&(ca_dac_config_t){ .channel = 1 });

    ca_stdout_printf_n("EX31: DMA M2M + M2P DAC1 started");
    ca_stdout_printf_n("EX31: DMA -> DAC1_OUT1 sine ~1.56Hz (DAC itself tested by ex27)");
    ca_stdout_printf_n("EX31: M2M verifies 1024B every second");
}


void ca_os_main(void)
{
    ca_os_job("dma_m2m",   CA_TH_LEVEL3, task_dma_m2m,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("dma_dac",   CA_TH_LEVEL3, task_dma_dac,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("heartbeat", CA_TH_LEVEL3, task_heartbeat,  NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_run(true);
}

#else /* EN_DMA && EN_DAC not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m31a, "=== Example 31 ===");
PRIVATE_STRING(m31b, "EN_DMA / EN_DAC == 0 : no DMA tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m31a);
    PUT_N(&priv_m31b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_DMA && EN_DAC */
