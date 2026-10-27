// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex61.c
 * @brief          : Example 61 — LPTIM blink low-power (periodic + one-shot)
 ******************************************************************************
 * @details
 * The user LED blinks on LPTIM1 (kernel clock LSI 32 kHz), alternating
 * two phases in a loop:
 *
 *   phase A — periodic 500 ms × 20 toggles (~10 s, slow blink)
 *   pause   — one-shot 2000 ms with LED off (the timer stops by itself)
 *   phase B — periodic 100 ms × 50 toggles (~5 s, fast blink)
 *   phase C — (if EN_PWR) periodic 500 ms with wake=true: the MCU enters
 *             STOP and the LPTIM wakes it up at every ARR match — the LED
 *             blinks with the CPU stopped (~10 toggles, then it resumes)
 *
 * The toggle occurs in the ISR callback (ARR match); the task orchestrates the
 * transitions by counting the fires. In phase C, the callback runs upon wake,
 * immediately after the pwr clock reinit; the CDC console is suspended during Stop.
 *
 * Supported boards (user LED):
 *   NUCLEO-U575ZI-Q  LD2 = PB7          NUCLEO-H563ZI  LD1 = PB0
 *   NUCLEO-H753ZI    LD1 = PB0          NUCLEO-F746ZG  LD1 = PB0
 *   F4VE: LPTIM not present on F407 → message and exit.
 *
 * CLI: "lptim info" shows mode and CNT of the active timer.
 ******************************************************************************
 */

#include "caos.h"

/* F1xx and F4xx have no LPTIM at all - not the F407, not the F429, not the
 * F446: the peripheral is absent from their CMSIS headers, from RM0390 and from
 * the datasheets, and hw/f{1,4}xx/drivers/lptim_*.c say so in their own header
 * ("stub: LPTIM not present"). They only exist because every family has to
 * implement the driver's externs.
 *
 * EN_LPTIM means "the ca_lptim_* API is compiled", not "there is hardware
 * behind it" - the same split as EN_RNG vs MCU_HAS_RNG. With the flag on, this
 * example registered its CLI commands and every one of them answered "LPTIM<n>
 * start error (free or invalid)", which reads like an allocation problem rather
 * than a part that simply does not have the peripheral. */
/* Tested on MCU_ID, not on MCU: the family names are not numeric macros, so
 * "MCU == F4xx" compares 0 with 0 and is true for every part. */
#if MCU_ID == 103 || MCU_ID == 407 || MCU_ID == 429 || MCU_ID == 446
#define EX61_NO_LPTIM_HW  1
#endif

#if EN_LPTIM && EN_PWR && !defined(EX61_NO_LPTIM_HW)

/* ============================================================================
 * Board configuration
 * ========================================================================== */

/* The LED pin used to be selected here per MCU_ID; it now comes from the
 * board's objmap.h through ca_led_*, which is why this example builds for
 * every part without an #error branch.
 * NOTE on the F4VE (F407): LPTIM is absent, so init still fails at runtime. */

/* ============================================================================
 * Demo parameters (modifiable constants)
 * ========================================================================== */

#define LPTIM_NUM        1U     /* LPTIM1 */
#define SLOW_PERIOD_MS 500U     /* phase A */
#define SLOW_TOGGLES    20U
#define FAST_PERIOD_MS 100U     /* phase B */
#define FAST_TOGGLES    50U
#define PAUSE_MS      2000U     /* one-shot between phases */
#define STOP_PERIOD_MS 500U     /* phase C: wake from STOP at every ARR match */
#define STOP_TOGGLES    10U

static volatile u32  s_fires;      /* toggles performed in the current phase */
static volatile bool s_oneshot_hit;

/* ISR callback: toggle LED and count */
static void cb_blink(void)
{
    ca_led_toggle(1U);
    s_fires++;
}

/* one-shot ISR callback: signal end of pause */
static void cb_pause_done(void)
{
    s_oneshot_hit = true;
}

/* Start a periodic phase and wait for n_toggles fires */
static bool run_phase(u32 period_ms, u32 n_toggles)
{
    s_fires = 0U;
    if (!ca_lptim_init_periodic(LPTIM_NUM, period_ms, cb_blink, false)) return false;
    ca_lptim_start(LPTIM_NUM);
    while (s_fires < n_toggles)
        ca_os_sleep(50);
    ca_lptim_stop(LPTIM_NUM);
    ca_lptim_deinit(LPTIM_NUM);
    return true;
}

/* Pause with LED off via one-shot */
static bool run_pause(u32 ms)
{
    s_oneshot_hit = false;
    ca_led_off(1U);
    if (!ca_lptim_init_oneshot(LPTIM_NUM, ms, cb_pause_done, false)) return false;
    ca_lptim_start(LPTIM_NUM);
    while (!s_oneshot_hit)
        ca_os_sleep(50);
    ca_lptim_deinit(LPTIM_NUM);   /* slot already released by dispatch: no-op safe */
    return true;
}

/* Phase C: periodic wake-armed — the MCU stays in STOP, the LPTIM wakes it up
 * at every ARR match (callback upon wake) and the task immediately re-enters STOP. */
static bool run_stop_phase(u32 period_ms, u32 n_toggles)
{
    s_fires = 0U;
    if (!ca_lptim_init_periodic(LPTIM_NUM, period_ms, cb_blink, true)) return false;
    ca_lptim_start(LPTIM_NUM);
    while (s_fires < n_toggles) {
        /* wake_flags=0: the pwr accepts because the LPTIM is wake-armed */
        if (ca_pwr_enter(CA_PWR_MODE_STOP, 0, 0) != 0) {
            ca_lptim_deinit(LPTIM_NUM);
            return false;
        }
    }
    ca_lptim_stop(LPTIM_NUM);
    ca_lptim_deinit(LPTIM_NUM);
    EVAL_USB( ca_os_sleep(500); )   /* let the CDC console re-enumerate */
    return true;
}

static u32 task_blink(void *arg)
{
    (void)arg;

    /* Banner printed here (task context): ca_os_configure runs before the
     * scheduler, where ca_stdout_* has no current context (TLB=NULL). */
    EVAL_STDOUT(
        ca_stdout_put_n("\r\n=== Example 61: LPTIM blink low-power ===");
        ca_stdout_printf_n("LPTIM%u on LSI 32 kHz: A=%u ms, B=%u ms, pause %u ms",
                           (u32)LPTIM_NUM, SLOW_PERIOD_MS, FAST_PERIOD_MS, PAUSE_MS);
        ca_stdout_printf_n("phase C: STOP with LPTIM wake every %u ms (console suspended)",
                           STOP_PERIOD_MS);
        ca_stdout_put_n("CLI: 'lptim info' for state and CNT");
    )

    ca_led_init();

    /* Probe: on F1/F4 the LPTIM does not exist and the init fails */
    if (!ca_lptim_init_oneshot(LPTIM_NUM, 100U, NULL, false)) {
        EVAL_STDOUT( ca_stdout_put_n("ex61: LPTIM not present on this MCU"); )
        return 1;
    }
    ca_lptim_deinit(LPTIM_NUM);

    while (1) {
        EVAL_STDOUT( ca_stdout_printf_n("ex61: phase A - periodic %u ms x %u",
                                        SLOW_PERIOD_MS, SLOW_TOGGLES); )
        if (!run_phase(SLOW_PERIOD_MS, SLOW_TOGGLES)) return 1;

        EVAL_STDOUT( ca_stdout_printf_n("ex61: pause - one-shot %u ms", PAUSE_MS); )
        if (!run_pause(PAUSE_MS)) return 1;

        EVAL_STDOUT( ca_stdout_printf_n("ex61: phase B - periodic %u ms x %u",
                                        FAST_PERIOD_MS, FAST_TOGGLES); )
        if (!run_phase(FAST_PERIOD_MS, FAST_TOGGLES)) return 1;

        EVAL_STDOUT( ca_stdout_printf_n("ex61: pause - one-shot %u ms", PAUSE_MS); )
        if (!run_pause(PAUSE_MS)) return 1;

        EVAL_STDOUT( ca_stdout_printf_n("ex61: phase C - STOP, wake LPTIM %u ms x %u",
                                        STOP_PERIOD_MS, STOP_TOGGLES); )
        ca_os_sleep(200);   /* flush stdout queue before stopping everything */
        if (!run_stop_phase(STOP_PERIOD_MS, STOP_TOGGLES)) return 1;
        EVAL_STDOUT( ca_stdout_put_n("ex61: phase C done (blink happened in STOP)"); )
    }
    return 0;
}

/* ============================================================================
 * Entry points
 * ========================================================================== */

void ca_os_configure(void)
{
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    EVAL_USB_CDC_STD( ca_usb_dev_mng_cdc_cli(); )
}

void ca_os_main(void)
{
    ca_lptim_init();   /* register CLI "lptim" */
    ca_os_job("lpblink", CA_TH_LEVEL3, task_blink, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* no LPTIM hardware, or EN_LPTIM / EN_PWR not both enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m61a, "=== Example 61 ===");
#ifdef EX61_NO_LPTIM_HW
PRIVATE_STRING(m61b, "no LPTIM on this family (F1xx/F4xx): example not supported");
#else
PRIVATE_STRING(m61b, "EN_LPTIM / EN_PWR disabled");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m61a);
    PUT_N(&priv_m61b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_LPTIM && EN_PWR && LPTIM hardware */
