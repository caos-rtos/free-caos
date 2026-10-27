// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 25 — Timer IRQ: quadrature generator + Timer Encoder
 *
 * Hardware (Nucleo-144: H753ZI MB1364, U575ZI-Q MB1549, F446ZE MB1137):
 *
 *   TIM3  periodic 100 µs (10 kHz)
 *         ISR: every 4 pulses generates a quadrature cycle on PB8/PB9
 *
 *   PB8   (GPIO output) — Signal A (quadrature) - CN7 pin 2
 *   PB9   (GPIO output) — Signal B (quadrature, 90° phase shifted) - CN7 pin 4
 *
 *   Encoder timer — counts the pulses on its CH1/CH2 inputs:
 *     H753: TIM4 on PB6/PB7 AF2   (CN10 pin 14 / pin 16)
 *     U575: TIM1 on PE9/PE11 AF1  (CN10 pin 4  / pin 6)
 *     F446: TIM1 on PE9/PE11 AF1  (CN10 pin 4  / pin 6)
 *           On U575 and F446 PB7 is the blue LD2, so TIM4_CH2 cannot be an input.
 *
 *   Direction:
 *     - CW  (ClockWise)      → A leads B → counter increments
 *     - CCW (CounterClockWise) → B leads A → counter decrements
 *
 * Wiring (2 jumpers) — the exact pins are printed by the "show" CLI command:
 *   H753:  PB8 ──── PB6   |   PB9 ──── PB7
 *   U575:  PB8 ──── PE9   |   PB9 ──── PE11
 *   F446:  PB8 ──── PE9   |   PB9 ──── PE11
 *
 * Visual output:
 *   LD — blink at 20 Hz (confirms TIM3 is running)
 *
 * Board pins/LEDs live in the per-MCU block below; the body uses only labels.
 *
 * Application CLI commands:
 *   setcw   — Set CW direction (clockwise)
 *   setccw  — Set CCW direction (counter-clockwise)
 *   show    — Show encoder position and statistics
 * Driver CLI commands (ca_timer_init):
 *   timer info | timer info <n> | timer start/stop/free/cnt <n>
 *
 * STDOUT Log (every 5 s):
 *   EX25: pos=NNNN  dir=CW  gen=NNNN  enc=NNNN
 */

#include "caos.h"

#if EN_TIMER

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */

#define TIM_GEN            3U           /* TIM3 — 10 kHz generator */

/* ============================================================================
 * PER-MCU MAPPING — quadrature output pins, encoder inputs and LED
 *
 * Only this block changes between boards; the body below uses the labels.
 * The encoder is hardwired to CH1/CH2 of its timer (ca_timer_hw_init_encoder
 * programs CCMR1.CC1S/CC2S), so CH3/CH4 pins are NOT an option: the encoder
 * timer must be one whose CH1/CH2 land on free pins.
 * ========================================================================== */

#if MCU_ID == 753
/* NUCLEO-H753ZI (MB1364) — TIM4_CH1/CH2 on PB6/PB7 AF2.
 * LED UM2407 §6.5: LD2 yellow PE1.
 *
 * CN10 pin numbers per UM2407 Table 21: PB6 is pin 14 (D1) and PB7 is pin 16
 * (D0). They read 13 and 15 here, which on this board are PG6 (QSPI_CS) and
 * PB2 (QSPI_CLK) - two pins that have nothing to do with TIM4. The MCU pins
 * were always right; only the connector positions were wrong, and those are
 * the half a person needs to fit the jumper, so `wire` reported both links
 * missing on a bench that was wired exactly as printed. */
#define TIM_QUAD_A_PORT    CA_GPIO_PORT_B  /* PB8 — GPIO output (signal A), CN7 pin 2 */
#define TIM_QUAD_A_PIN     CA_GPIO_PIN_8
#define TIM_QUAD_B_PORT    CA_GPIO_PORT_B  /* PB9 — GPIO output (signal B), CN7 pin 4 */
#define TIM_QUAD_B_PIN     CA_GPIO_PIN_9

#define TIM_ENC            4U           /* TIM4 — Encoder mode */
#define TIM_ENC_A_PORT     CA_GPIO_PORT_B  /* PB6 — TIM4_CH1 (input A), CN10 pin 14 */
#define TIM_ENC_A_PIN      CA_GPIO_PIN_6
#define TIM_ENC_A_AF       2U
#define TIM_ENC_B_PORT     CA_GPIO_PORT_B  /* PB7 — TIM4_CH2 (input B), CN10 pin 16 */
#define TIM_ENC_B_PIN      CA_GPIO_PIN_7
#define TIM_ENC_B_AF       2U
#define ENC_TXT            "TIM4 CH1:PB6 CN10-14  CH2:PB7 CN10-16  AF2"
#define WIRE_A_TXT         "PB8 (CN7 pin 2) ---- PB6 (CN10 pin 14)  [signal A]"
#define WIRE_B_TXT         "PB9 (CN7 pin 4) ---- PB7 (CN10 pin 16)  [signal B]"


#elif MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549) — the encoder canNOT stay on TIM4_CH1/CH2 here:
 * PB7 is the blue LD2, driven through a transistor (UM2861 §6.4), so it is
 * unusable as an encoder INPUT (ex60 drives that same pin as a PWM output,
 * which is the opposite direction and works).
 * TIM1 is used instead: CH1/CH2 are PE9/PE11, both routed to the Zio connector
 * CN10 (pins 4 and 6, D6/D5 — UM2861 Table 18) with no conflict.
 * The generator keeps PB8/PB9: they are I2C_A_SCL/SDA on D15/D14 (CN7 pin 2/4)
 * but free as plain GPIO outputs.
 * LED UM2861 §6.4: LD2 blue PB7 — driven by the generator here, so the visual
 * confirmation LED is LD1 green PC7 instead. */
#define TIM_QUAD_A_PORT    CA_GPIO_PORT_B  /* PB8 — GPIO output (signal A), CN7 pin 2 */
#define TIM_QUAD_A_PIN     CA_GPIO_PIN_8
#define TIM_QUAD_B_PORT    CA_GPIO_PORT_B  /* PB9 — GPIO output (signal B), CN7 pin 4 */
#define TIM_QUAD_B_PIN     CA_GPIO_PIN_9

#define TIM_ENC            1U           /* TIM1 — Encoder mode */
#define TIM_ENC_A_PORT     CA_GPIO_PORT_E  /* PE9  — TIM1_CH1 (input A), CN10 pin 4 */
#define TIM_ENC_A_PIN      CA_GPIO_PIN_9
#define TIM_ENC_A_AF       1U           /* AF1 = TIM1_CH1 */
#define TIM_ENC_B_PORT     CA_GPIO_PORT_E  /* PE11 — TIM1_CH2 (input B), CN10 pin 6 */
#define TIM_ENC_B_PIN      CA_GPIO_PIN_11
#define TIM_ENC_B_AF       1U           /* AF1 = TIM1_CH2 */
#define ENC_TXT            "TIM1 CH1:PE9 CN10-4   CH2:PE11 CN10-6  AF1"
#define WIRE_A_TXT         "PB8 (CN7 pin 2) ---- PE9  (CN10 pin 4)  [signal A]"
#define WIRE_B_TXT         "PB9 (CN7 pin 4) ---- PE11 (CN10 pin 6)  [signal B]"


#elif MCU_ID == 446
/* NUCLEO-F446ZE (MB1137), pins from UM1974 Table 18. Same trap as the U575:
 * the generic TIM4 CH1/CH2 pair is PB6/PB7 and PB7 is the blue LD2 here
 * (UM1974 §7.5), so it cannot be an encoder INPUT. TIM1 is used instead:
 * PE9/PE11 are listed as TIM1_CH1/TIM1_CH2 and brought out as D6/D5 on CN10,
 * exactly like the U575 - but AF1 on the F series (TIM1/TIM2 are AF1 here,
 * whereas the same signal is AF14 on the U5: AFs do not carry across families).
 * The generator stays on PB8/PB9, free on CN7. */
#define TIM_QUAD_A_PORT    CA_GPIO_PORT_B
#define TIM_QUAD_A_PIN     CA_GPIO_PIN_8
#define TIM_QUAD_B_PORT    CA_GPIO_PORT_B
#define TIM_QUAD_B_PIN     CA_GPIO_PIN_9
#define TIM_ENC            1U
#define TIM_ENC_A_PORT     CA_GPIO_PORT_E
#define TIM_ENC_A_PIN      CA_GPIO_PIN_9
#define TIM_ENC_A_AF       1U           /* AF1 = TIM1_CH1 */
#define TIM_ENC_B_PORT     CA_GPIO_PORT_E
#define TIM_ENC_B_PIN      CA_GPIO_PIN_11
#define TIM_ENC_B_AF       1U           /* AF1 = TIM1_CH2 */
#define ENC_TXT            "TIM1 CH1:PE9 CN10-4   CH2:PE11 CN10-6  AF1"
#define WIRE_A_TXT         "PB8 (CN7 pin 2) ---- PE9  (CN10 pin 4)  [signal A]"
#define WIRE_B_TXT         "PB9 (CN7 pin 4) ---- PE11 (CN10 pin 6)  [signal B]"

#elif MCU_ID == 563
/* NUCLEO-H563ZI (UM3115 Tables 21 and 24, DS14258 Table 15).
 * Generator PB8/PB9 on CN7; encoder TIM4_CH1/CH2 on PB6/PB7 AF2, which the
 * datasheet confirms for this part - the AF the generic #else assumed happens
 * to be right here, unlike ex24.
 *
 * What was missing is the connectors: PB6 and PB7 come out on CN10, not CN7,
 * so the two jumpers cross between headers. PB7 is NOT a LED on this board
 * (LD1/LD2/LD3 are PB0/PF4/PG4 in objmap.h), which is the trap the U575 branch
 * had to work around.
 *
 * PB6/PB7 are D1/D0, shared with LPUART1 on the Zio header: nothing here uses
 * LPUART1, but do not wire this example and a LPUART1 test at the same time. */
#define TIM_QUAD_A_PORT    CA_GPIO_PORT_B  /* PB8 - GPIO output (signal A), CN7 pin 2 */
#define TIM_QUAD_A_PIN     CA_GPIO_PIN_8
#define TIM_QUAD_B_PORT    CA_GPIO_PORT_B  /* PB9 - GPIO output (signal B), CN7 pin 4 */
#define TIM_QUAD_B_PIN     CA_GPIO_PIN_9
#define TIM_ENC            4U
#define TIM_ENC_A_PORT     CA_GPIO_PORT_B  /* PB6 - TIM4_CH1 (input A), CN10 pin 14 */
#define TIM_ENC_A_PIN      CA_GPIO_PIN_6
#define TIM_ENC_A_AF       2U           /* AF2 = TIM4_CH1 */
#define TIM_ENC_B_PORT     CA_GPIO_PORT_B  /* PB7 - TIM4_CH2 (input B), CN10 pin 16 */
#define TIM_ENC_B_PIN      CA_GPIO_PIN_7
#define TIM_ENC_B_AF       2U           /* AF2 = TIM4_CH2 */
#define ENC_TXT            "TIM4 CH1:PB6 CN10-14  CH2:PB7 CN10-16  AF2"
#define WIRE_A_TXT         "PB8 CN7-2 ---- PB6 CN10-14  [signal A]"
#define WIRE_B_TXT         "PB9 CN7-4 ---- PB7 CN10-16  [signal B]"

#elif MCU_ID == 767
/* NUCLEO-F767ZI (MB1137), pins from UM1974 Table 18 and DS11532 Table 13.
 *
 * The generic #else below would have put the encoder on PB6/PB7 AF2, and the
 * AFs are right for this part - but PB7 IS the blue LD2 on this board and
 * appears in NO connector table of UM1974: a jumper cannot reach it. That is
 * exactly the trap the U575 branch had to work around, and it would have been
 * invisible until the wire test failed on a pin nobody can wire.
 *
 * So CH2 moves to PD13, which the UM lists as D28 on CN10-19, a plain I/O, and
 * DS11532 Table 13 gives as TIM4_CH2 on AF2 (PD12 is CH1, PD14 CH3, PD15 CH4).
 * CH1 stays on PB6: D26 on CN10-13, free here - unlike the H563, where the same
 * pin sits at CN10-14.
 * PD12/PD13 are also offered as A4/A5 analog inputs on CN9 through solder
 * bridges; the plain I/O row on CN10 is the one used here. */
#define TIM_QUAD_A_PORT    CA_GPIO_PORT_B  /* PB8 - GPIO output (signal A), CN7 pin 2 */
#define TIM_QUAD_A_PIN     CA_GPIO_PIN_8
#define TIM_QUAD_B_PORT    CA_GPIO_PORT_B  /* PB9 - GPIO output (signal B), CN7 pin 4 */
#define TIM_QUAD_B_PIN     CA_GPIO_PIN_9
#define TIM_ENC            4U
#define TIM_ENC_A_PORT     CA_GPIO_PORT_B  /* PB6  - TIM4_CH1 (input A), CN10 pin 13 */
#define TIM_ENC_A_PIN      CA_GPIO_PIN_6
#define TIM_ENC_A_AF       2U           /* AF2 = TIM4_CH1 */
#define TIM_ENC_B_PORT     CA_GPIO_PORT_D  /* PD13 - TIM4_CH2 (input B), CN10 pin 19 */
#define TIM_ENC_B_PIN      CA_GPIO_PIN_13
#define TIM_ENC_B_AF       2U           /* AF2 = TIM4_CH2 */
#define ENC_TXT            "TIM4 CH1:PB6 CN10-13  CH2:PD13 CN10-19  AF2"
#define WIRE_A_TXT         "PB8 CN7-2 ---- PB6  CN10-13  [signal A]"
#define WIRE_B_TXT         "PB9 CN7-4 ---- PD13 CN10-19  [signal B]"

#else
/* Other boards: generator on PB8/PB9, encoder on TIM4 CH1/CH2 (PB6/PB7 AF2),
 * which is the common mapping on the F series. Verify against the board UM —
 * in particular that PB7 is not wired to a LED, as it is on the U575 — and add
 * a proper block above once confirmed on hardware. */
#define TIM_QUAD_A_PORT    CA_GPIO_PORT_B
#define TIM_QUAD_A_PIN     CA_GPIO_PIN_8
#define TIM_QUAD_B_PORT    CA_GPIO_PORT_B
#define TIM_QUAD_B_PIN     CA_GPIO_PIN_9
#define TIM_ENC            4U
#define TIM_ENC_A_PORT     CA_GPIO_PORT_B
#define TIM_ENC_A_PIN      CA_GPIO_PIN_6
#define TIM_ENC_A_AF       2U
#define TIM_ENC_B_PORT     CA_GPIO_PORT_B
#define TIM_ENC_B_PIN      CA_GPIO_PIN_7
#define TIM_ENC_B_AF       2U
#define ENC_TXT            "TIM4 CH1:PB6 CH2:PB7 AF2 (check in the UM)"
#define WIRE_A_TXT         "PB8 ---- PB6  [signal A]  (check pins)"
#define WIRE_B_TXT         "PB9 ---- PB7  [signal B]  (check pins)"
#endif

/* TIM3 period: 100 µs (10 kHz) */
#define TIM3_PERIOD_US     100U

/* Every 4 TIM3 pulses = 1 full quadrature cycle (2.5 kHz) */
#define QUAD_DIVISOR       4U

/* ============================================================================
 * SHARED STATE (accessed by ISR → volatile)
 * ========================================================================== */

static volatile u32 g_tim3_irq_count = 0;    /* TIM3 interrupt count */
static volatile u32 g_quad_cycles = 0;       /* Generated quadrature cycles */
static volatile u8  g_phase = 0;             /* Current quadrature phase (0-3) */
static volatile u8  g_req_cw = 1;            /* 1 = CW, 0 = CCW (default CW) */

/* Encoder position counter (read from TIM8 CTR) */
static volatile i64 g_encoder_pos = 0;       /* Position read from encoder */
static i64 encoder_pos_accum = 0;            /* Accumulator for encoder read */

/* Statistics */
static volatile u32 g_cw_cycles = 0;         /* Generated CW cycles */
static volatile u32 g_ccw_cycles = 0;        /* Generated CCW cycles */

/* ============================================================================
 * TIM3 ISR CALLBACK — Quadrature signal generator
 * ========================================================================== */

static void cb_tim3_generator(void)
{
    g_tim3_irq_count++;

    /* Toggle LED every interrupt for visual confirmation (effective 20 Hz) */
    ca_led_toggle(2U);

    /* Generate quadrature signal every 4 TIM3 interrupts */
    static u8 quad_state = 0;

    if (g_tim3_irq_count % QUAD_DIVISOR == 0) {
        g_quad_cycles++;

        /* Quadrature sequence (4 states) */
        if (g_req_cw) {
            /* CW: 00 → 10 → 11 → 01 → 00 */
            switch (quad_state) {
                case 0:  /* 00 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 0);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 0);
                    break;
                case 1:  /* 10 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 1);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 0);
                    break;
                case 2:  /* 11 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 1);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 1);
                    break;
                case 3:  /* 01 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 0);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 1);
                    break;
            }
            g_cw_cycles++;
        } else {
            /* CCW: 00 → 01 → 11 → 10 → 00 */
            switch (quad_state) {
                case 0:  /* 00 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 0);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 0);
                    break;
                case 1:  /* 01 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 0);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 1);
                    break;
                case 2:  /* 11 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 1);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 1);
                    break;
                case 3:  /* 10 */
                    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 1);
                    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 0);
                    break;
            }
            g_ccw_cycles++;
        }

        quad_state = (quad_state + 1) % 4;
    }
}

/* ============================================================================
 * MONITOR TASK — Read encoder position and statistics
 * ========================================================================== */

static u32 task_monitor(void *arg)
{
    (void)arg;

    /* Banner here, NOT in ca_os_configure: that runs before the scheduler,
     * where ca_stdout_* has no task context and prints "TLB=NULL". */
    EVAL_STDOUT(
        ca_stdout_put_n("\r\n=== Example 25: Quadrature Encoder ===");
        ca_stdout_put_n("TIM3: 10 kHz generator (100 us)");
        ca_stdout_put_n("Quadrature: 2.5 kHz on PB8/PB9");
        ca_stdout_put_n("Encoder  : " ENC_TXT);
        ca_stdout_put_n("Connect  : " WIRE_A_TXT);
        ca_stdout_put_n("           " WIRE_B_TXT);
        ca_stdout_put_n("Type 'help' for commands  (default direction: CW)");
    )

    while (1) {
        ca_os_sleep(5000);

        /* Read and update position from encoder */
        ca_timer_encoder_read(TIM_ENC);
        g_encoder_pos = encoder_pos_accum;

        i64 pos = g_encoder_pos;
        u32 tim3_cnt = g_tim3_irq_count;
        u32 quad_cnt = g_quad_cycles;
        u8 dir = g_req_cw;
        u32 cw_cnt = g_cw_cycles;
        u32 ccw_cnt = g_ccw_cycles;

        EVAL_STDOUT(
            ca_stdout_printf_n("EX25: pos=%d  dir=%s  tim3_irq=%u  quad_cycles=%u  cw=%u  ccw=%u",
                              (long)pos, dir ? "CW" : "CCW",
                              (unsigned)tim3_cnt, (unsigned)quad_cnt,
                              (unsigned)cw_cnt, (unsigned)ccw_cnt);
        )
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
        /* One line listing the bare commands, like the other examples that
         * register theirs bare (ex21 "hello | status | show | wire", ex22,
         * ex24). Four descriptive lines said the same thing in a shape of
         * their own; ex64 keeps its "s64: ..." prefix because there the
         * prefix is real - it dispatches on strncmp(p, "s64", 3). */
        CLI_PUT_N("setcw | setccw | show | wire");
        return false;
    }
    if (ca_strncmp(p, "wire", 4) == 0) {
        /* Continuity test of the two jumpers, on plain GPIO: no timer, no
         * encoder, so a FAIL here means the wire, not the configuration.
         * TIM3 must be STOPPED first - its ISR drives these very pins 10000
         * times a second and would overwrite every level before the read.
         * The encoder timer goes too: it owns the input pins as AF. */
        ca_timer_stop(TIM_GEN);
        ca_timer_stop(TIM_ENC);

        static const struct { ca_gpio_port_t op; ca_gpio_pin_t opin;
                              ca_gpio_port_t ip; ca_gpio_pin_t ipin; const char *name; }
        L[] = {
            { TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, TIM_ENC_A_PORT, TIM_ENC_A_PIN,
              WIRE_A_TXT },
            { TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, TIM_ENC_B_PORT, TIM_ENC_B_PIN,
              WIRE_B_TXT },
        };
        u32 good = 0U;
        for (u32 i = 0; i < 2U; i++) {
            ca_gpio_configure(L[i].op, L[i].opin, CA_GPIO_MODE_OUTPUT,
                              CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
            ca_gpio_configure(L[i].ip, L[i].ipin, CA_GPIO_MODE_INPUT,
                              CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
            ca_gpio_write(L[i].op, L[i].opin, 0);
            for (volatile u32 d = 0; d < 20000U; d++) { }
            u8 lo = ca_gpio_read(L[i].ip, L[i].ipin);
            ca_gpio_write(L[i].op, L[i].opin, 1);
            for (volatile u32 d = 0; d < 20000U; d++) { }
            u8 hi = ca_gpio_read(L[i].ip, L[i].ipin);
            CLI_PRINTF_N("  %s : drive0->read%u  drive1->read%u  %s",
                         L[i].name, lo, hi,
                         (lo == 0 && hi == 1) ? "OK (wire present)"
                                              : "FAIL (wire missing or swapped)");
            if (lo == 0 && hi == 1) good++;
        }
        /* One line the bench can read, identical in every example that has a
         * `wire` command - see the note in ex64. */
        CLI_PRINTF_N("  WIRE RESULT : %s  (%u/2)",
                     (good == 2U) ? "OK" : "FAIL", good);
        /* The pins are plain GPIO now, not timer AF: reboot to put the
         * generator and the encoder back the way ca_os_configure() set them. */
        CLI_PUT_N("  rebooting in 2.5 s to restore the timers...");
        ca_os_sleep(2500);
        ca_os_reset();
        return true;
    }
    if (ca_strncmp(p, "setcw", 5) == 0 && (p[5] == '\0' || p[5] == ' ')) {
        g_req_cw = 1;
        CLI_PUT_N("direction: CW");
        return true;
    }
    if (ca_strncmp(p, "setccw", 6) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        g_req_cw = 0;
        CLI_PUT_N("direction: CCW");
        return true;
    }
    if (ca_strncmp(p, "show", 4) == 0 && (p[4] == '\0' || p[4] == ' ')) {
        ca_timer_encoder_read(TIM_ENC);
        i32 pos      = (i32)encoder_pos_accum;
        u32 tim3_cnt = g_tim3_irq_count;
        u32 quad_cnt = g_quad_cycles;
        u8  dir      = g_req_cw;
        u32 cw_cnt   = g_cw_cycles;
        u32 ccw_cnt  = g_ccw_cycles;
        i32 expected = dir ? (i32)quad_cnt : -(i32)quad_cnt;

        CLI_PUT_N("=== QUADRATURE ENCODER STATUS ===");
        CLI_PUT_N("  wiring    : " WIRE_A_TXT);
        CLI_PUT_N("              " WIRE_B_TXT);
        CLI_PRINTF_N("  pos       : %d", pos);
        CLI_PRINTF_N("  dir       : %s", dir ? "CW" : "CCW");
        CLI_PRINTF_N("  tim3_irq  : %u", tim3_cnt);
        CLI_PRINTF_N("  quad_cyc  : %u", quad_cnt);
        CLI_PRINTF_N("  cw_cyc    : %u", cw_cnt);
        CLI_PRINTF_N("  ccw_cyc   : %u", ccw_cnt);
        CLI_PRINTF_N("  expected  : %d", expected);
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
    /* ========================================================================
     * 1. Initialize GPIO for quadrature signal (output)
     * ======================================================================== */

    /* PB8: GPIO output — Signal A - CN7 pin 2 */
    ca_gpio_init_port(TIM_QUAD_A_PORT);
    ca_gpio_configure(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN,
                      CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, 0U);
    ca_gpio_write(TIM_QUAD_A_PORT, TIM_QUAD_A_PIN, 0);

    /* PB9: GPIO output — Signal B - CN7 pin 4 */
    ca_gpio_init_port(TIM_QUAD_B_PORT);
    ca_gpio_configure(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN,
                      CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, 0U);
    ca_gpio_write(TIM_QUAD_B_PORT, TIM_QUAD_B_PIN, 0);

    /* ========================================================================
     * 2. Initialize GPIO for encoder input (TIM4 CH1/CH2)
     * ======================================================================== */

    /* PB6: AF2 (TIM4_CH1) — Encoder channel A input - CN10 pin 14 */
    ca_gpio_init_port(TIM_ENC_A_PORT);
    ca_gpio_configure(TIM_ENC_A_PORT, TIM_ENC_A_PIN,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_HIGH, CA_GPIO_PULLUP, TIM_ENC_A_AF);

    /* PB7: AF2 (TIM4_CH2) — Encoder channel B input - CN10 pin 16 */
    ca_gpio_init_port(TIM_ENC_B_PORT);
    ca_gpio_configure(TIM_ENC_B_PORT, TIM_ENC_B_PIN,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_HIGH, CA_GPIO_PULLUP, TIM_ENC_B_AF);

    /* ========================================================================
     * 3. Initialize yellow LED LD2 (visual confirmation of TIM3)
     * ======================================================================== */

    ca_led_init();
    ca_led_off(2U);

    /* ========================================================================
     * 4. Initialize TIM3: 10 kHz generator (100 µs)
     * ======================================================================== */

    ca_timer_init_periodic(TIM_GEN, TIM3_PERIOD_US, cb_tim3_generator);
    ca_timer_start(TIM_GEN);

    /* ========================================================================
     * 5. Initialize TIM8: Encoder mode (counting on both edges)
     * ======================================================================== */

    ca_timer_init_encoder(TIM_ENC, &encoder_pos_accum);
    ca_timer_start(TIM_ENC);

    /* ========================================================================
     * 6. Initialize system modules (sensors, log, CLI, USB)
     * ======================================================================== */


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
PRIVATE_STRING(m25a, "=== Example 25 ===");
PRIVATE_STRING(m25b, "EN_TIMER == 0 (skipping quadrature encoder example)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m25a);
    PUT_N(&priv_m25b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_TIMER */
