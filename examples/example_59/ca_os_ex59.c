// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex59.c
 * @brief          : Example 59 — power manager (pwr) interactive test
 ******************************************************************************
 * @details
 * SILENT example: no task prints autonomously, all output is
 * on request via CLI. Designed for pwr driver validation:
 *
 *   pwr info          power status (sysclk, vos, standby_resume, peripherals)
 *   pwr freq <1-99>   frequency reduction (HPRE) + retune tick/uart/can
 *   pwr restore       nominal frequency restore
 *   pwr alarm <s>     RTC alarm test in RUN (no stop)
 *   pwr sleep <s>     simple WFI, wake from RTC alarm
 *   pwr stop <s>      STOP, wake from RTC alarm
 *   pwr stopms <ms>   STOP, wake from LPTIM1 on LSI in milliseconds
 *                     (U5xx only, resolution up to 31 µs, max 262144 ms)
 *   pwr dbg           low-power register dump (U5xx only, requires EN_PWR_DBG)
 *   pwr standby <s>   STANDBY, wake from RTC alarm = reset
 *   rtc info          current RTC time
 *
 * A silent heartbeat task runs in the background (verifies that the
 * scheduler survives stop/wake cycles): counter visible with "task 1".
 ******************************************************************************
 */

#include "caos.h"

#if EN_PWR && EN_RTC && EN_CLI_PWR && EN_CLI_RTC

/* Heartbeat LED for board: blinking = scheduler alive. Distinguishes "OS ok,
 * console dead only" from "wake failed" when the console does not respond.
 * The pin comes from objmap.h, so every board that declares a LED gets one. */

static u32 heartbeat(void *arg) {
    (void)arg;
    ca_led_init();
    while (1) {
        ca_os_sleep(1000);
        ca_led_toggle(1U);
    }
    return 0;
}

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
    EVAL_PWR( ca_pwr_init(); )
    EVAL_RTC( ca_rtc_init(CA_RTC_CLK_AUTO); )   /* wake source for sleep/stop/standby */
    EVAL_USB_CDC_STD( ca_usb_dev_mng_cdc_cli(); )
}

void ca_os_main(void) {
    ca_os_job("hbeat", CA_TH_LEVEL3, heartbeat, NULL,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* EN_PWR && EN_RTC not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m59a, "=== Example 59 ===");
PRIVATE_STRING(m59b, "EN_PWR / EN_CLI_PWR / EN_RTC / EN_CLI_PWR disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m59a);
    PUT_N(&priv_m59b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_PWR || EN_RTC */
