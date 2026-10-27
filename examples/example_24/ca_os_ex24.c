// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 24 — Timer IRQ: 10 kHz generator + external countdown counter
 *
 * Hardware (Nucleo-144: H753ZI MB1364, U575ZI-Q MB1549, F446ZE MB1137):
 *
 *   TIM3  periodic 100 µs (10 kHz)
 *         ISR: toggle the signal pin (PD0 on all three boards)
 *
 *   TIM2  counter-irq, external clock via ETR = PA0 (TIM2_ETR, AF per board)
 *         preset = 10000 pulses → IRQ every 1 s
 *         ISR: increment g_1s_irq, toggle LD1
 *
 * Wiring (1 wire, same on all boards):
 *   PD0 (CN9 pin 25)  ────  PA0 (CN10 pin 29)
 *
 * Visual output:
 *   LD2 — blink at 20 Hz (confirms TIM3 running)
 *   LD1 — blink every 1 s (confirms ETR + TIM2)
 *
 * Board pins/LEDs live in the per-MCU block below; the body uses only labels.
 *
 * STDOUT Log (every 5 s):
 *   EX24: t=5 s  10khz_irq=100000  1s_irq=5
 */

#include "caos.h"

#if EN_TIMER

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */

#define TIM_GEN       3U          /* TIM3 — generator */
#define TIM_CNT       2U          /* TIM2 — external countdown counter */

/* ETR counts only RISING edges of the output pin: pulses_ETR = TIM3_Hz / 2
 * DEBUG:      20 Hz TIM3 → 10 edges/s, preset=10  → IRQ every 1 s
 * PRODUCTION: 10 kHz TIM3 → 5000 edges/s, preset=5000 → IRQ every 1 s  */
#define PERIOD_GEN    100U      
#define PRESET_CNT    5000U        

/* ============================================================================
 * PER-MCU MAPPING — signal pin, TIM2 ETR input and LEDs
 *
 * Only this block changes between boards; the body below uses the labels.
 * TIM2_ETR is PA0 on every board here, but the AF is NOT the same: AF1 on the
 * H753 and F446, AF14 on the U575. Check the datasheet, never assume.
 * ========================================================================== */

#if MCU_ID == 753
/* NUCLEO-H753ZI (MB1364).
 * PD0 is CAN_RX/D67 on CN9 pin 25 (UM2407 Table 20), PA0 is D32 on CN10 pin 29
 * (Table 21).
 * PA0 reaches CN10-29 only with SB75 ON, and B1 contends for it: the factory
 * default puts B1 on PC13 (SB51 ON / SB58 OFF, UM2407 §6.4), so PA0 is free.
 * LEDs UM2407 §6.5: LD1 green PB0, LD2 yellow PE1. */
#define GPIO_OUT_PORT  CA_GPIO_PORT_D    /* signal out: PD0 */
#define GPIO_OUT_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_PORT  CA_GPIO_PORT_A    /* TIM2_ETR: PA0 AF1 */
#define GPIO_ETR_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_AF    1U
#define WIRE_TXT       "PD0 CN9-25 (TIM3 out) ---- PA0 CN10-29 (TIM2_ETR)"
#define GEN_TXT        "TIM3 -> PD0 CN9-25  (GPIO toggle)"
#define ETR_TXT        "TIM2_ETR <- PA0 CN10-29  AF1  (needs SB75 ON)"
#define LED_TXT        "LD1 green PB0 (1 s)   LD2 yellow PE1 (20 Hz)"

#elif MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549).
 * PD0 is CAN_RX/D67 on CN9 pin 25 — same pin as the H753, free as a GPIO here.
 * PA0 is D32 on CN10 pin 29 (UM2861 Table 18; PD0 in Table 17). TIM2_ETR is
 * AF14 there, NOT AF1
 * as on the H753 — AF1 on this part is TIM2_CH1 (DS13737 Rev 10 Table 27/28).
 * The USER button sits on PC13 by default (SB58 ON / SB59 OFF, UM2861 §7.6),
 * so PA0 is free.
 * LEDs UM2861 §6.4: LD1 green PC7, LD2 blue PB7. */
#define GPIO_OUT_PORT  CA_GPIO_PORT_D    /* signal out: PD0 */
#define GPIO_OUT_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_PORT  CA_GPIO_PORT_A    /* TIM2_ETR: PA0 AF14 (DS13737 Table 28) */
#define GPIO_ETR_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_AF    14U            /* NOT AF1: on U575 PA0/AF1 is TIM2_CH1 */
#define WIRE_TXT       "PD0 CN9-25 (TIM3 out) ---- PA0 CN10-29 (TIM2_ETR)"
#define GEN_TXT        "TIM3 -> PD0 CN9-25  (GPIO toggle)"
#define ETR_TXT        "TIM2_ETR <- PA0 CN10-29  AF14 (NOT AF1 as on H753)"
#define LED_TXT        "LD1 green PC7 (1 s)   LD2 blue PB7 (20 Hz)"

#elif MCU_ID == 446
/* NUCLEO-F446ZE (MB1137).
 * PD0 is CAN_RX/D67 on CN9 pin 25, PA0 is D32 on CN10 pin 29 (UM1974 Table 18).
 * TIM2_ETR is AF1 here, as on the H753: DS stm32f446 Table 11 gives PA0/AF1 as
 * TIM2_CH1/TIM2_ETR (AF2 is TIM5_CH1, AF3 TIM8_ETR).
 * PA0 reaches CN10-29 only with SB179 ON, and the B1 button contends for it:
 * the factory default puts B1 on PC13 (SB173 ON / SB180 OFF, UM1974 §7.4), so
 * PA0 is free — same situation as the U575 above.
 * LEDs UM1974 §7.5: LD1 green PB0, LD2 blue PB7, all active HIGH. */
#define GPIO_OUT_PORT  CA_GPIO_PORT_D    /* signal out: PD0 */
#define GPIO_OUT_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_PORT  CA_GPIO_PORT_A    /* TIM2_ETR: PA0 AF1 */
#define GPIO_ETR_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_AF    1U
#define WIRE_TXT       "PD0 CN9-25 (TIM3 out) ---- PA0 CN10-29 (TIM2_ETR)"
#define GEN_TXT        "TIM3 -> PD0 CN9-25  (GPIO toggle)"
#define ETR_TXT        "TIM2_ETR <- PA0 CN10-29  AF1  (needs SB179 ON)"
#define LED_TXT        "LD1 green PB0 (1 s)   LD2 blue PB7 (20 Hz)"

#elif MCU_ID == 767
/* NUCLEO-F767ZI (MB1137) - the same board as the F446 above, so the connector
 * pins and solder bridges are identical: PD0 is CN9-25, PA0 is CN10-29 with
 * SB179 ON, and B1 sits on PC13 by default, leaving PA0 free.
 *
 * The AF is the one that had to be looked up rather than assumed, since it is
 * what the H563 branch below warns about: DS11532 Table 13 gives PA0/AF1 as
 * TIM2_CH1/TIM2_ETR on this part (AF2 is TIM5_CH1, AF3 TIM8_ETR), so AF1 like
 * the H753 and the F446 - NOT the AF14 that U575 and H563 need.
 * LEDs UM1974 7.5: LD1 green PB0, LD2 blue PB7. */
#define GPIO_OUT_PORT  CA_GPIO_PORT_D    /* signal out: PD0 */
#define GPIO_OUT_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_PORT  CA_GPIO_PORT_A    /* TIM2_ETR: PA0 AF1 */
#define GPIO_ETR_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_AF    1U
#define WIRE_TXT       "PD0 CN9-25 (TIM3 out) ---- PA0 CN10-29 (TIM2_ETR)"
#define GEN_TXT        "TIM3 -> PD0 CN9-25  (GPIO toggle)"
#define ETR_TXT        "TIM2_ETR <- PA0 CN10-29  AF1  (needs SB179 ON)"
#define LED_TXT        "LD1 green PB0 (1 s)   LD2 blue PB7 (20 Hz)"

#elif MCU_ID == 563
/* NUCLEO-H563ZI (UM3115 Tables 23 and 24, DS14258 Tables 15 and 16).
 * PD0 is CAN_RX/D67 on CN9 pin 25, PA0 is TIMER_C_PWM1/D32 on CN10 pin 29 -
 * the same physical pairing as H753 and U575, so the jumper does not move.
 *
 * The AF does: PA0/AF1 is TIM2_CH1 on this part (Table 15), and TIM2_ETR is
 * PA0/AF14 (Table 16). Wiring it as AF1 leaves the pin on capture/compare and
 * the external clock never reaches the counter - 1s_irq stays at 0 and 'wire'
 * reports drive0->read1 drive1->read1, a pin nobody drives. Exactly the trap
 * the U575 branch above documents, and the reason the generic #else must not
 * be trusted for a new board (H563, 2026-08-28). */
#define GPIO_OUT_PORT  CA_GPIO_PORT_D    /* signal out: PD0 */
#define GPIO_OUT_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_PORT  CA_GPIO_PORT_A    /* TIM2_ETR: PA0 AF14 */
#define GPIO_ETR_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_AF    14U            /* NOT AF1: on H563 PA0/AF1 is TIM2_CH1 */
#define WIRE_TXT       "PD0 CN9-25 (TIM3 out) ---- PA0 CN10-29 (TIM2_ETR)"
#define GEN_TXT        "TIM3 -> PD0 CN9-25  (GPIO toggle)"
#define ETR_TXT        "TIM2_ETR <- PA0 CN10-29  AF14"
#define LED_TXT        "LD1 PB0 / LD2 PF4"

#else
/* Other boards: PD0 -> PA0 is the usual pairing and TIM2_ETR is AF1 on the F
 * series, but the LEDs differ — check the board UM before wiring, and add a
 * proper block above once the pins are confirmed on hardware. */
#define GPIO_OUT_PORT  CA_GPIO_PORT_D
#define GPIO_OUT_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_PORT  CA_GPIO_PORT_A
#define GPIO_ETR_PIN   CA_GPIO_PIN_0
#define GPIO_ETR_AF    1U
#define WIRE_TXT       "PD0 (TIM3 out) ---- PA0 (TIM2_ETR)  [connectors: see the board UM]"
#define GEN_TXT        "TIM3 -> PD0  (CN? — check the board UM)"
#define ETR_TXT        "TIM2_ETR <- PA0  AF1 (to be checked)  (CN? — check the board UM)"
#define LED_TXT        "LD1 / LD2 — check the board UM for this part"
#endif

/* ============================================================================
 * SHARED STATE (accessed from ISR → volatile)
 * ========================================================================== */

static volatile u32 g_10khz_irq = 0;
static volatile u32 g_1s_irq    = 0;

/* ============================================================================
 * ISR CALLBACKS
 * ========================================================================== */

static void cb_10khz(void)
{
    ca_gpio_toggle(GPIO_OUT_PORT, GPIO_OUT_PIN);
    ca_led_toggle(2U);
    g_10khz_irq++;
}

static void cb_1sec(void)
{
    g_1s_irq++;
    ca_led_toggle(1U);
}

/* ============================================================================
 * MONITOR TASK
 * ========================================================================== */

static u32 task_monitor(void *arg)
{
    (void)arg;

    while (1) {
        ca_os_sleep(5000);
        u32 hz  = g_10khz_irq;
        u32 sec = g_1s_irq;
        EVAL_STDOUT( ca_stdout_printf_n("EX24: os_ms=%u  10khz_irq=%u  1s_irq=%u",
                                      (unsigned)ca_os_tick_ms(), (unsigned)hz, (unsigned)sec); )
    }
    return 0;
}

/* ============================================================================
 * CLI — local to this example
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("info | wire");
        return false;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PUT_N("--- Example 24: TIM3 generator + TIM2 ETR counter ---");
        CLI_PUT_N("  wiring   : " WIRE_TXT);
        CLI_PUT_N("  generator: " GEN_TXT);
        CLI_PUT_N("  counter  : " ETR_TXT);
        CLI_PUT_N("  leds     : " LED_TXT "  (on-board, no connector)");
        CLI_PRINTF_N("  preset   : %u ETR edges -> 1 IRQ", (u32)PRESET_CNT);
        CLI_PRINTF_N("  10khz_irq: %u", (u32)g_10khz_irq);
        CLI_PRINTF_N("  1s_irq   : %u", (u32)g_1s_irq);
        return true;
    }
    if (ca_strncmp(p, "wire", 4) == 0) {
        /* Continuity test of the single jumper. TIM3 must be STOPPED first:
         * its ISR toggles the very pin we are driving, 10000 times a second,
         * so every level written here would be overwritten before the read
         * and the test would report FAIL on a perfectly good jumper. */
        ca_timer_stop(TIM_GEN);
        ca_timer_stop(TIM_CNT);

        ca_gpio_configure(GPIO_OUT_PORT, GPIO_OUT_PIN, CA_GPIO_MODE_OUTPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
        ca_gpio_configure(GPIO_ETR_PORT, GPIO_ETR_PIN, CA_GPIO_MODE_INPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);

        ca_gpio_write(GPIO_OUT_PORT, GPIO_OUT_PIN, 0);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 lo = ca_gpio_read(GPIO_ETR_PORT, GPIO_ETR_PIN);
        ca_gpio_write(GPIO_OUT_PORT, GPIO_OUT_PIN, 1);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 hi = ca_gpio_read(GPIO_ETR_PORT, GPIO_ETR_PIN);

        CLI_PUT_N("  " WIRE_TXT);
        CLI_PUT_N("    out: " GEN_TXT);
        CLI_PUT_N("    in : " ETR_TXT);
        CLI_PRINTF_N("  drive0->read%u  drive1->read%u  %s", lo, hi,
                     (lo == 0 && hi == 1) ? "OK (jumper present)"
                                          : "FAIL (jumper missing/wrong)");
        /* One line the bench can read, identical in every example that has a
         * `wire` command - see the note in ex64. */
        CLI_PRINTF_N("  WIRE RESULT : %s  (%u/1)",
                     (lo == 0 && hi == 1) ? "OK" : "FAIL",
                     (u32)((lo == 0 && hi == 1) ? 1U : 0U));
        /* TIM3/TIM2 are stopped and the pins are plain GPIO now: there is no
         * way back to the measurement from here, so reboot rather than asking
         * for the button. The delay leaves the result on screen. */
        CLI_PUT_N("  rebooting in 2.5 s to go back to the timer...");
        ca_os_sleep(2500);
        ca_os_reset();
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ============================================================================
 * ENTRY POINTS
 * ========================================================================== */

void ca_os_configure(void)
{
    /* PD0: fast push-pull output → 10 kHz signal */
    ca_gpio_init_port(GPIO_OUT_PORT);
    ca_gpio_configure(GPIO_OUT_PORT, GPIO_OUT_PIN,
                      CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, 0U);

    /* PA0: AF1 (TIM2_ETR) → external clock input TIM2 */
    ca_gpio_init_port(GPIO_ETR_PORT);
    ca_gpio_configure(GPIO_ETR_PORT, GPIO_ETR_PIN,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_HIGH, CA_GPIO_PULLUP, GPIO_ETR_AF);

    /* LED 1 blinks at 1 Hz from TIM2, LED 2 at 20 Hz from TIM3 (visual
     * confirmation that the generator runs). Pins come from objmap.h. */
    ca_led_init();

    /* TIM3: 20 Hz generator (debug) */
    ca_timer_init_periodic(TIM_GEN, PERIOD_GEN, cb_10khz);
    ca_timer_start(TIM_GEN);

    /* TIM2: countdown 10 ETR rising edges @ 20Hz/2=10Hz = 1 s (debug) */
    ca_timer_init_counter_irq(TIM_CNT, PRESET_CNT, cb_1sec);
    ca_timer_start(TIM_CNT);

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
    ca_timer_init();
    ca_os_job("monitor", CA_TH_LEVEL3, task_monitor, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}

#else  /* EN_TIMER == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m24a, "=== Example 24 ===");
PRIVATE_STRING(m24b, "EN_TIMER == 0 (skipping timer example)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m24a);
    PUT_N(&priv_m24b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_TIMER */
