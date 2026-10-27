// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex90.c
 * @brief          : Example 90 — CLI echo on the USB CDC console
 ******************************************************************************
 * @details
 * Proves that, with the console on USB CDC, a character typed by the user
 * never lands INSIDE a line printed by another task.
 *
 * The CLI echo is produced in the RX path (here the USB CDC callback, through
 * ca_cli_rx_byte) and queued with ca_stdout_echo(): the stdout drain is the
 * only writer of the console and flushes the echo only BETWEEN lines. This was
 * verified on the UART console (U575, H563); this example covers the CDC one.
 *
 * Idle at boot. "e90 start" starts a "talker" task that prints a fixed-format
 * line every EX90_PERIOD_MS, "e90 stop" silences it:
 *
 *     EX90 seq=<n> tick=<ms> END
 *
 * Type on the CDC console while it runs. Typed characters may appear BETWEEN
 * talker lines (accepted, as with a background job in bash) but never inside
 * one: every line holding "EX90 seq=" must end with the exact format above.
 * The check is the format, not the eye.
 *
 * HOW TO RUN IT
 * -------------
 * Plug the USB device connector; the console is the CDC port (VID ca01),
 * not the ST-LINK VCP. Emulated typing at human pace:
 *
 *   CAOS_TTY=/dev/ttyACMn CAOS_CHAR_MS=120 caos_shell -i < commands.txt
 *
 * CLI: "e90 info"       - lines printed, period, stdout and CDC counters
 *      "e90 period N"   - talker period in ms (5..1000)
 *      "e90 stop|start" - silence the talker, e.g. to read diagnostics
 *      "e90 line N"     - one line of exactly N chars (1..127): N=62/126 are
 *                         64/128-byte USB writes, the zero-length-packet case
 *      "cli_stat"       - 'echo dropped' must stay 0
 */

#include "caos.h"

#if MCU_ID == 575 || MCU_ID == 753 || MCU_ID == 407 || MCU_ID == 446 \
    || MCU_ID == 767 || MCU_ID == 563
#define EX90_OK  (EN_USB && EN_USB_CDC && STDOUT_ON_USB_CDC && ENABLE_CLI)
#else
/* example not supported by hardware */
#define EX90_OK  0
#endif

#if EX90_OK

#define EX90_PERIOD_MS   20U
#define EX90_PERIOD_MIN  5U
#define EX90_PERIOD_MAX  1000U
#define EX90_LINE_MAX    127U   /* ca_stdout_put_n payload, before "\r\n" */

static volatile u32 s_period = EX90_PERIOD_MS;
static volatile u32 s_seq;
static volatile bool s_run;            /* idle until "e90 start" */
static volatile u32 s_tx_refused;

/* Console sender: STDOUT (ca_usb_cdc_write) plus a count of its refusals.
 * A refusal is not a loss - the stdout drain re-offers the line up to
 * STDOUT_SENDER_RETRIES ticks before dropping it - but it tells how often the
 * CDC endpoint ring was full. */
static u16 ex90_put(p_str m)
{
    if (STDOUT(m)) return 1U;
    s_tx_refused++;
    return 0U;
}

/* Prints one fixed-format line per period, only while a terminal is open */
static u32 task_talker(void *arg)
{
    (void)arg;
    while (!ca_usb_cdc_connected()) ca_os_sleep(100);
    ca_os_sleep(1000);
    ca_stdout_put_n("\r\n=== Example 90: CLI echo on the USB CDC console ===");
    /* Idle until asked: say so, or the silence looks like a hang. */
    ca_stdout_put_n("ex90: idle - \"e90 start\" starts the talker, then type:"
                    " no char may land inside an 'EX90 seq=' line");
    ca_stdout_put_n("CLI: 'e90 start|stop|info|period N|line N', 'cli_stat'");

    while (1) {
        ca_os_sleep(s_period);
        if (!s_run || !ca_usb_cdc_connected()) continue;
        ca_stdout_printf_n("EX90 seq=%u tick=%u END", s_seq, ca_os_tick_ms());
        s_seq++;
    }
    return 0;
}

/* One line of exactly n chars, "\r\n" excluded: "L<nnn>:0123...#".
 * n = 62 makes a 64-byte USB write, n = 126 a 128-byte one: a write that is a
 * whole number of packets is where a CDC that forgets the closing zero-length
 * packet leaves the host waiting for more. */
static void ex90_line(u32 n)
{
    char buf[EX90_LINE_MAX + 1U];
    u32 i;
    for (i = 0; i < n; i++) buf[i] = (char)('0' + (i % 10U));
    if (n >= 5U) {
        buf[0] = 'L';
        buf[1] = (char)('0' + (n / 100U) % 10U);
        buf[2] = (char)('0' + (n / 10U) % 10U);
        buf[3] = (char)('0' + n % 10U);
        buf[4] = ':';
    }
    buf[n - 1U] = '#';
    buf[n] = '\0';
    ca_stdout_put_n(buf);
}

static bool ex90_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e90       : info | period N | stop | start | line N");
        return false;
    }
    if (ca_strncmp(p, "e90", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "info", 4) == 0) {
        u32 queued, dropped;
        ca_stdout_stats(&queued, &dropped);
        CLI_PRINTF_N("lines  : %u", s_seq);
        CLI_PRINTF_N("period : %u ms", s_period);
        CLI_PRINTF_N("stdout : queued %u  dropped %u  echo dropped %u",
                     queued, dropped, ca_stdout_echo_dropped());
        CLI_PRINTF_N("cdc tx : refused %u", s_tx_refused);
        return true;
    }
    if (ca_strncmp(p, "line", 4) == 0) {
        i32 v = ca_strtol(ca_skip_sp(p + 4), 10);
        if (v < 1 || v > (i32)EX90_LINE_MAX) {
            CLI_PRINTF_N("e90: line 1..%u", EX90_LINE_MAX);
            return true;
        }
        ex90_line((u32)v);
        return true;
    }
    if (ca_strncmp(p, "stop", 4) == 0) {
        s_run = false;
        CLI_PUT_N("e90: talker stopped");
        return true;
    }
    if (ca_strncmp(p, "start", 5) == 0) {
        s_run = true;
        CLI_PUT_N("e90: talker running");
        return true;
    }
    if (ca_strncmp(p, "period", 6) == 0) {
        i32 v = ca_strtol(ca_skip_sp(p + 6), 10);
        if (v < (i32)EX90_PERIOD_MIN || v > (i32)EX90_PERIOD_MAX) {
            CLI_PRINTF_N("e90: period %u..%u ms", EX90_PERIOD_MIN, EX90_PERIOD_MAX);
            return true;
        }
        s_period = (u32)v;
        CLI_PRINTF_N("period : %u ms", s_period);
        return true;
    }
    CLI_PUT_N("e90: info | period N | stop | start | line N");
    return true;
}

static ca_cli_node_t s_ex90_cli_node = { ex90_cli_handler, NULL };

void ca_os_configure(void)
{
    ca_stdout_init();
    ca_stdout_set_sender(ex90_put);
    ca_stdout_create_thread();

    ca_cli_init(2048);
    ca_cli_set_sender(ex90_put);
    ca_cli_create_thread();

    ca_usb_dev_mng_cdc_cli();
    ca_cli_register(&s_ex90_cli_node);
}

void ca_os_main(void)
{
    ca_os_job("talker", CA_TH_LEVEL4, task_talker, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* !EX90_OK */

/* The console of this config is the USB CDC, which may never enumerate on a
 * part where the example does not run: the bench marker goes out on the
 * debug UART directly, as in ex63. */
PRIVATE_STRING(m90a, "=== Example 90 ===");
PRIVATE_STRING(m90b, "needs USB CDC console - not supported on this MCU or config");
PRIVATE_STRING(ca_unsup_90, CA_EXAMPLE_UNSUPPORTED_MSG);

static u16 unsup_put(p_str message)
{
    return ca_uart_put(UART_DBG_NUM, message);
}

void ca_os_configure(void)
{
    ca_uart_init(UART_DBG_NUM, CA_UART_BAUD_115200, NULL, 0);
    ca_put_n(unsup_put, &priv_ca_unsup_90);
    ca_put_n(unsup_put, &priv_m90a);
    ca_put_n(unsup_put, &priv_m90b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EX90_OK */
