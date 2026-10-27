// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex100.c
 * @brief          : Example 100 — timer input capture of the LSE, no wire
 ******************************************************************************
 * @details
 * Every family routes the LSE (32768 Hz crystal) to one timer input for
 * calibration: TIM16 CH1 on U575/H563/H753, TIM5 CH4 on F446/F767. The timer
 * captures one rising edge every 8 and the callback adds up the ticks.
 *
 *   capture test   1 s of captures: the count must be ~4096 and the measured
 *                  LSE within 1000 ppm of 32768 Hz. The crystal is good to
 *                  ~20 ppm, so the error is really the CPU clock's (U575: its
 *                  MSI is locked to the LSE, ~0). Also checks that a timer
 *                  without the internal route refuses it.
 *                  Ends with "IC TEST PASS" or "IC TEST FAIL".
 *
 * EN_RTC starts the LSE at boot (ca_rtc_clock_init).
 ******************************************************************************
 */

#include "caos.h"

#if EN_TIMER

#if MCU_ID == 575 || MCU_ID == 563 || MCU_ID == 753
#define IC_TIM      16U
#define IC_CH       1U
#elif MCU_ID == 446 || MCU_ID == 767
#define IC_TIM      5U
#define IC_CH       4U
#endif

#define IC_DIV      8U
#define IC_NO_ROUTE 2U          /* TIM2 CH1: no internal LSE route on any family */
#define LSE_HZ      32768U
#define PPM_TOL     1000

static volatile u32 s_ic_count;
static volatile u64 s_ic_ticks;

static void ic_cb(u32 ticks)        /* timer interrupt */
{
    s_ic_ticks += ticks;
    s_ic_count++;
}

static void ic_test(void)
{
    bool ok = true;

#ifdef IC_TIM
    s_ic_count = 0U;
    s_ic_ticks = 0U;
    if (!ca_timer_init_capture(IC_TIM, IC_CH, CA_TIMER_IC_LSE, IC_DIV, ic_cb)) {
        CLI_PRINTF_N("IC TIM%u CH%u: init refused", IC_TIM, IC_CH);
        CLI_PUT_N("IC TEST FAIL");
        return;
    }
    (void)ca_timer_start(IC_TIM);
    ca_os_sleep(1000);
    (void)ca_timer_stop(IC_TIM);
    const u32 n   = s_ic_count;
    const u64 sum = s_ic_ticks;
    const u32 clk = ca_timer_clock_hz(IC_TIM);
    (void)ca_timer_deinit(IC_TIM);

    /* f = clk * div * n / sum, in mHz to keep the ppm */
    const u64 f_mhz = (sum != 0U) ? ((u64)clk * IC_DIV * n * 1000U) / sum : 0U;
    const i32 ppm   = (i32)((((i64)f_mhz - (i64)LSE_HZ * 1000) * 1000) / (i64)LSE_HZ);
    const u32 want  = LSE_HZ / IC_DIV;                      /* captures in 1 s */
    const bool n_ok = n > want - want / 20U && n < want + want / 20U;
    const bool f_ok = ppm >= -PPM_TOL && ppm <= PPM_TOL;
    CLI_PRINTF_N("IC TIM%u CH%u LSE /%u: %u captures in 1 s (want ~%u), clock %u Hz",
                 IC_TIM, IC_CH, IC_DIV, n, want, clk);
    CLI_PRINTF_N("IC LSE measured %u.%03u Hz, %d ppm (limit %d)", (u32)(f_mhz / 1000U),
                 (u32)(f_mhz % 1000U), ppm, PPM_TOL);
    ok = n_ok && f_ok;
#else
    CLI_PUT_N("IC: no LSE route known for this part");
    ok = false;
#endif

    /* A channel without the internal route must say no, not capture a pin */
    const bool refused = !ca_timer_init_capture(IC_NO_ROUTE, 1U, CA_TIMER_IC_LSE, IC_DIV, ic_cb);
    if (!refused) (void)ca_timer_deinit(IC_NO_ROUTE);
    CLI_PRINTF_N("IC TIM%u CH1 LSE refused: %s", IC_NO_ROUTE, refused ? "yes" : "NO <-- FAIL");
    ok = ok && refused;

    CLI_PUT_N(ok ? "IC TEST PASS" : "IC TEST FAIL");
}

#if ENABLE_CLI
static bool ex100_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("capture: test"); return false; }
    if (ca_strncmp(p, "capture", 7) != 0) return false;
    p = ca_skip_sp(p + 7);
    if (ca_strncmp(p, "test", 4) == 0) { ic_test(); return true; }
    CLI_PUT_N("capture: test");
    return true;
}
static ca_cli_node_t s_cli = { ex100_cli, NULL };
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
        ca_cli_register(&s_cli);
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else

PRIVATE_STRING(m100a, "=== Example 100 ===");
PRIVATE_STRING(m100b, "ex100: EN_TIMER disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m100a);
    PUT_N(&priv_m100b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
