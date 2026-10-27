// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 3 — LED and GPIO CLI
 *
 * Hardware (Nucleo-H753ZI, MB1364):
 *   LD1 green  → PB0   (CN10 pin 31)
 *   LD2 yellow → PE1   (CN10 pin 25)
 *   LD3 red    → PB14  (CN10 pin 28)
 *
 * Hardware (NUCLEO-U575ZI-Q):
 *   LD1 green  → PC7
 *   LD2 blue   → PB7
 *   LD3 red    → PG2
 *
 * Hardware (NUCLEO-H563ZI):
 *   LD1 green  → PB0   (the only onboard LED)
 *   LD2/LD3    → PB1/PB2 (free pins, no physical LED — CLI compatibility only)
 *
 * Hardware (STM32F4VE / F407, black board):
 *   LD1 (D2)   → PA6   (the only onboard LED)
 *   LD2/LD3    → PB0/PB1 (free pins, no physical LED — CLI compatibility only)
 *
 * The monitor task blinks LD1 every 1 s.
 * Both command sets come from their driver, not from a local CLI hook: this
 * example registers no hook of its own.
 *
 * CLI commands (led.c driver, EN_CLI_LED=1):
 *   led info                — list the LEDs this board declares
 *   led on <n>              — light LED n     (n=1..count)
 *   led off <n>             — turn it off
 *   led tog <n>             — toggle it
 *
 * CLI commands (gpio_driver.c driver, EN_CLI_GPIO=1):
 *   gpio info <P> <n>       — mode, pull, speed, AF and level of one pin
 *   gpio all <P>            — the whole port at once
 *   gpio get <P> <n>        — read pin state
 *   gpio set <P> <n> <0|1>  — write pin
 *   gpio tog <P> <n>        — toggle pin
 *                             (P=A..K, n=0..15)
 *
 * The two are complementary and both are wanted here: 'led' works in the
 * board's own terms and needs no pinout, 'gpio' reaches any pin at all. On a
 * board that has just arrived, ex3 is usually the first thing flashed, so the
 * raw pin access belongs in the simplest example and not only in ex21/67/83.
 *
 * STDOUT log (every 1 s):
 *   EX3: tick=NNNNN  LD1=N
 */

#include "caos.h"

/* The LED pins used to live here, one #if MCU_ID branch per board. They now
 * come from the board's objmap.h through ca_led_*, so this example works on
 * any part without knowing which pins it carries - and boards with a single
 * LED (F407, F103) no longer need two made-up pins to keep the CLI building. */

/* ============================================================================
 * TASK MONITOR
 * ========================================================================== */

static u32 task_monitor(void *arg)
{
    (void)arg;

    ca_os_sleep(500);
    /* The board tells us how many LEDs it has, so the banner needs no #if */
    ca_stdout_printf_n("EX3: LED and GPIO CLI - this board has %u LED(s)",
                       (unsigned)ca_led_count());
    ca_stdout_put_n("Commands: led info | led on 1 | led off 1 | led tog 1");
    /* The gpio hints name port A only: which port carries a LED changes with
     * the board and ca_led_* does not expose it, so a suggested 'set' on a
     * guessed pin could drive something real. 'info'/'all' only read. */
    ca_stdout_put_n("          gpio info A 0 | gpio all A | gpio get A 0   (P=A..K, n=0..15)");

    while (1) {
        ca_os_sleep(1000);
        ca_led_toggle(1U);
        ca_stdout_printf_n("EX3: tick=%u  LED1=%u",
            (unsigned)ca_os_tick_ms(), (unsigned)ca_led_get(1U));
    }
    return 0;
}

/* ============================================================================
 * ENTRY POINTS
 * ========================================================================== */

void ca_os_configure(void)
{
    /* Clocks, pin configuration and "all off" for every LED this board has */
    ca_led_init();

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
}

void ca_os_main(void)
{
    ca_os_job("monitor", CA_TH_LEVEL3, task_monitor, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}
