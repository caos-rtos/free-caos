// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 18 — SW Timer (One-Shot and Periodic)
 *
 * Demonstrates the use of the caOS software timer system (ca_sw_timer_t).
 *
 * Scenario:
 *   - LED periodic timer: toggles LED every 500ms via callback
 *   - Stats periodic timer: prints statistics every 2s
 *   - "warmup" one-shot timer: fires once 3s after startup,
 *     then it is rearmed every time it receives the CLI command "arm"
 *   - Monitor task: shows status of active timers every 5s
 *
 * The callback is executed by the timer daemon task (not from ISR).
 * Do not allocate stack or perform blocking operations in callbacks.
 *
 * CLI COMMANDS:
 *   arm    — (re)arms the one-shot timer (3s)
 *   stop   — stops stats periodic timer
 *   start  — restarts stats periodic timer
 *   show   — shows timer status
 *   help   — command list
 *
 * os_config.h REQUIREMENTS:
 *   EN_SW_TIMER  1
 */

#include "caos.h"

#if EN_SW_TIMER == 1

/* ============================================================================
 * Board LED mapping (physical LED toggled by cb_led) — same scheme as ex.3
 * ========================================================================== */

/* The LED pin used to be picked here per MCU_ID. It now comes from the board's
 * objmap.h through ca_led_*, so there is nothing board-specific left and no
 * #error branch: a part without a LED simply reports a count of 0. */

/* ============================================================================
 * Handles and counters
 * ========================================================================== */

static ca_sw_timer_handle_t htim_led;     /* 500ms periodic */
static ca_sw_timer_handle_t htim_stats;   /* 2000ms periodic */
static ca_sw_timer_handle_t htim_oneshot; /* 3000ms one-shot */

static volatile u32 led_ticks;
static volatile u32 stats_ticks;
static volatile u32 oneshot_fired;
static volatile u8  led_state;

/* ============================================================================
 * Callbacks — executed by the timer daemon
 * ========================================================================== */

static void cb_led(void *arg) {
    (void)arg;
    led_state ^= 1u;
    led_ticks++;
    ca_led_toggle(1U);
}

static void cb_stats(void *arg) {
    (void)arg;
    stats_ticks++;
    ca_stdout_printf_n("STAT #%u  LED=%u  OS=%u",
        stats_ticks, led_ticks, oneshot_fired);
}

static void cb_oneshot(void *arg) {
    (void)arg;
    oneshot_fired++;
    ca_stdout_printf_n("ONE-SHOT fired! (#%u)", oneshot_fired);
}

/* ============================================================================
 * Monitor task
 * ========================================================================== */

static u32 task_monitor(void *arg) {
    (void)arg;
    ca_os_sleep(1000);
    while (1) {
        u8 active = ca_sw_timer_count_active();
        ca_sw_timer_state_t s1 = ca_sw_timer_get_state(htim_led);
        ca_sw_timer_state_t s2 = ca_sw_timer_get_state(htim_stats);
        ca_sw_timer_state_t s3 = ca_sw_timer_get_state(htim_oneshot);
        u32 rem3 = ca_sw_timer_get_remaining(htim_oneshot);

        ca_stdout_printf_n("=== TIMER STATUS (active=%u) ===", active);
        ca_stdout_printf_n("  led    : %s (fires=%u)",
            s1 == CA_TIMER_RUNNING ? "RUN" : "STOP", led_ticks);
        ca_stdout_printf_n("  stats  : %s (fires=%u)",
            s2 == CA_TIMER_RUNNING ? "RUN" : "STOP", stats_ticks);
        ca_stdout_printf_n("  oneshot: %s rem=%ums (fires=%u)",
            s3 == CA_TIMER_RUNNING ? "RUN" :
            s3 == CA_TIMER_EXPIRED ? "EXP" : "STOP",
            rem3, oneshot_fired);
        ca_stdout_printf_n("================================");
        ca_os_sleep(5000);
    }
    return 0;
}

/* ============================================================================
 * CLI — chain of responsibility
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("arm   - arm the one-shot (3s)");
        CLI_PUT_N("stop  - stop the stats timer");
        CLI_PUT_N("start - start the stats timer");
        CLI_PUT_N("show  - timer status");
        return false;
    }
    if (ca_strncmp(p, "arm", 3) == 0 && (p[3] == 0 || p[3] == ' ')) {
        ca_sw_timer_reset(htim_oneshot);
        ca_sw_timer_start(htim_oneshot);
        CLI_PUT_N("one-shot armed (3s)");
        return true;
    }
    if (ca_strncmp(p, "stop", 4) == 0 && (p[4] == 0 || p[4] == ' ')) {
        ca_sw_timer_stop(htim_stats);
        CLI_PUT_N("timer stats stopped");
        return true;
    }
    if (ca_strncmp(p, "start", 5) == 0 && (p[5] == 0 || p[5] == ' ')) {
        ca_sw_timer_start(htim_stats);
        CLI_PUT_N("timer stats started");
        return true;
    }
    if (ca_strncmp(p, "show", 4) == 0 && (p[4] == 0 || p[4] == ' ')) {
        CLI_PUT_N("--- Example 18 ---");
        CLI_PRINTF_N("led ticks    : %u", (u32)led_ticks);
        CLI_PRINTF_N("stats fires  : %u", (u32)stats_ticks);
        CLI_PRINTF_N("oneshot fires: %u", (u32)oneshot_fired);
        CLI_PRINTF_N("active timers: %u", (u32)ca_sw_timer_count_active());
        return true;
    }
    return false;
}

static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ============================================================================
 * ca_os_configure
 * ========================================================================== */

void ca_os_configure(void) {
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
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}

/* ============================================================================
 * ca_os_main
 * ========================================================================== */

void ca_os_main(void) {
    led_ticks     = 0;
    stats_ticks   = 0;
    oneshot_fired = 0;
    led_state     = 0;

    /* Physical LED toggled by cb_led */
    ca_led_init();
    ca_led_off(1U);

    /* Timer system init (creates daemon task) */
    ca_sw_timer_init();

    /* LED periodic timer: 500ms, auto-repeated */
    htim_led = ca_sw_timer_create(500, CA_TIMER_PERIODIC, cb_led, NULL);
    ca_sw_timer_start(htim_led);

    /* Stats periodic timer: 2000ms */
    htim_stats = ca_sw_timer_create(2000, CA_TIMER_PERIODIC, cb_stats, NULL);
    ca_sw_timer_start(htim_stats);

    /* One-shot timer: 3000ms, not started immediately */
    htim_oneshot = ca_sw_timer_create(3000, CA_TIMER_ONE_SHOT, cb_oneshot, NULL);
    ca_sw_timer_start(htim_oneshot);  /* starts immediately, then rearmable via CLI */

    /* Monitor task */
    ca_os_job("monitor", CA_TH_LEVEL1, task_monitor, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}

#else /* EN_SW_TIMER == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m18a, "=== Example 18 ===");
PRIVATE_STRING(m18b, "EN_SW_TIMER ==0, skipping software timer example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m18a);
    PUT_N(&priv_m18b);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_SW_TIMER */
