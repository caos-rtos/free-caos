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
 * calibration: TIM16 CH1 on U575/H563/H753, TIM5 CH4 on F407/F439/F446/F767. The timer
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
#elif MCU_ID == 407 || MCU_ID == 439 || MCU_ID == 446 || MCU_ID == 767
#define IC_TIM      5U
#define IC_CH       4U
#elif MCU_ID == 773
/* H7Rx/7Sx: no LSE input on TIM16; LSE -> MCO1 -> TIM15 CH1 (timer_h7rsxx.c) */
#define IC_TIM      15U
#define IC_CH       1U
#endif

/* Capture prescaler: 8 LSE periods between captures must stay under the
 * 16-bit counter. At 240 MHz (H753) that is 58593 counts; at 300 MHz (H773,
 * TIM15 on APB2 x2) it would be 73242 and wrap - /4 there. */
#if MCU_ID == 773
#define IC_DIV      4U
#else
#define IC_DIV      8U
#endif
#define IC_NO_ROUTE 2U          /* TIM2 CH1: no internal LSE route on any family */
#define LSE_HZ      32768U
/* Known FAIL on the bench NUCLEO-H753ZI, left on purpose: its HSE is the
 * ST-LINK-V3E MCO (UM2407 7.5.1, SB45 ON, X3 not fitted) and that clock is
 * off: -479 ppm on 2026-10-24, -4400..-5000 ppm and drifting on 2026-10-29,
 * same with another cable, USB port and after a power cycle. uptime vs PC
 * confirmed the CPU is the fast one (+4700 ppm), the LSE is good. The
 * limit is not widened: the test reports a real board defect. */
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
/* capture — the line arrives without the command word */
static bool capture_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "test", 4) == 0) { ic_test(); return true; }
    return false;                   /* cli prints the help line */
}
static const ca_cli_cmd_t s_capture_cmds[] = {
    { "capture", "capture   : test", { capture_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_capture_table = {
    "capture", s_capture_cmds, (u16)(sizeof(s_capture_cmds) / sizeof(s_capture_cmds[0])), NULL
};
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
        ca_cli_register_table(&s_capture_table);
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
