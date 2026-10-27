// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 28 — RTC: setting, reading, alarms with callbacks
 *
 * Hardware: NUCLEO-H753ZI and NUCLEO-U575ZI-Q
 * RTC Clock: CA_RTC_CLK_AUTO - the 32.768 kHz LSE crystal (every Nucleo-144
 *            carries one, H753ZI included) when it is found running at boot,
 *            else the LSI calibrated against the SysTick (less accurate,
 *            temperature-dependent: expect drift over long runs).
 *
 * Tasks:
 *   task_display  — prints date/time every 5 seconds via STDOUT
 *   task_alarm    — responds to alarm callbacks (via semaphore)
 *
 * Alarms:
 *   Alarm A — periodic: triggers every minute when seconds return to 0
 *   Alarm B — one-shot:  triggers at the time set via CLI, then disables itself
 *
 * CLI Commands (rtc.c driver):
 *   rtc info                     — read current date/time
 *   rtc set YYYY-MM-DD HH:MM:SS  — set date/time
 *   rtc alarm <a|b> HH:MM:SS     — set alarm A or B
 *   rtc cal <ppm>                — calibrate RTC (e.g. rtc cal -10)
 *
 * STDOUT Log (every 5 s):
 *   EX28: Mon 21 Mar 2026  12:00:05
 *
 * REQUIREMENTS os_config.h:
 *   EN_RTC 1
 */

#include "caos.h"

#if EN_RTC

/* ============================================================================
 * CONFIGURATION — initial date/time
 * ========================================================================== */

#define INIT_YEAR   26u   /* 2026 */
#define INIT_MONTH   3u
#define INIT_DAY    21u
#define INIT_WDAY    5u   /* 1=Mon … 7=Sun */
#define INIT_HOUR   12u
#define INIT_MIN     0u
#define INIT_SEC     0u

/* ============================================================================
 * SHARED STATE
 * ========================================================================== */

static ca_semaphore_t   g_alarm_sem;
static volatile u8   g_alarm_last = 0;

/* ============================================================================
 * HELPERS
 * ========================================================================== */

static const char * const wday_str[]  = { "", "Lun", "Mar", "Mer", "Gio", "Ven", "Sab", "Dom" };
static const char * const month_str[] = { "", "Gen", "Feb", "Mar", "Apr", "Mag", "Giu",
                                               "Lug", "Ago", "Set", "Ott", "Nov", "Dic" };

/* ============================================================================
 * ALARM CALLBACKS — called from IRQ, only set semaphore
 * ========================================================================== */

static void on_alarm(u8 alarm_id)
{
    g_alarm_last = alarm_id;
    ca_sem_post(&g_alarm_sem);
}

/* ============================================================================
 * DISPLAY TASK — prints date/time every 5 seconds
 * ========================================================================== */

static u32 task_display(void *arg)
{
    (void)arg;
    ca_rtc_dt_t dt;

    while (1) {
        if (ca_rtc_get(&dt)) {
            ca_stdout_printf_n("EX28: %s %02u %s 20%02u  %02u:%02u:%02u",
                wday_str[dt.wday & 7u],
                (unsigned)dt.day,
                month_str[dt.month <= 12u ? dt.month : 0u],
                (unsigned)dt.year,
                (unsigned)dt.hour, (unsigned)dt.min, (unsigned)dt.sec);
        } else {
            ca_stdout_put_n("EX28: RTC read error");
        }
        ca_os_sleep(5000);
    }
    return 0;
}

/* ============================================================================
 * ALARM TASK — waits for semaphore and prints the event
 * ========================================================================== */

static u32 task_alarm(void *arg)
{
    (void)arg;
    ca_rtc_dt_t dt;

    while (1) {
        ca_sem_wait(&g_alarm_sem);

        u8 id = g_alarm_last;
        ca_rtc_get(&dt);

        if (id == CA_RTC_ALARM_A) {
            ca_stdout_printf_n("*** ALARM A ***  %02u:%02u:%02u  (every minute)",
                (unsigned)dt.hour, (unsigned)dt.min, (unsigned)dt.sec);
            /* Alarm A remains active — no disable action */
        } else {
            ca_stdout_printf_n("*** ALARM B ***  %02u:%02u:%02u  (one-shot, disabled)",
                (unsigned)dt.hour, (unsigned)dt.min, (unsigned)dt.sec);
            ca_rtc_clear_alarm(CA_RTC_ALARM_B);
        }
    }
    return 0;
}

/* ============================================================================
 * ENTRY POINTS
 * ========================================================================== */

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
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    /* LSE if the 32.768 kHz crystal is found running, else LSI */
    ca_rtc_init(CA_RTC_CLK_AUTO);

    /* Set initial date/time */
    ca_rtc_dt_t dt = {
        .sec   = INIT_SEC,   .min   = INIT_MIN,   .hour  = INIT_HOUR,
        .wday  = INIT_WDAY,  .day   = INIT_DAY,
        .month = INIT_MONTH, .year  = INIT_YEAR
    };
    ca_rtc_set(&dt);

    /* Alarm A — periodic: every minute (match only seconds == 0) */
    ca_rtc_alarm_t alarm_a = {
        .sec        = 0,
        .match_sec  = true,
        .match_min  = false,
        .match_hour = false,
        .match_day  = false,
    };
    ca_rtc_set_alarm(CA_RTC_ALARM_A, &alarm_a, on_alarm);

    /* Semaphore for alarm callbacks */
    ca_sem_init(&g_alarm_sem,"EX28_S1", 0, 8);
}

void ca_os_main(void)
{
    ca_os_job("display", CA_TH_LEVEL3, task_display, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("alarm",   CA_TH_LEVEL2, task_alarm,   NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);


    ca_os_run(true);
}

#else /* EN_RTC == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m28a, "=== Example 28 ===");
PRIVATE_STRING(m28b, "EN_RTC == 0, skipping RTC example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m28a);
    PUT_N(&priv_m28b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_RTC */
