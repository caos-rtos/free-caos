// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 27 — DAC output + ADC readback (polling and ISR)
 *
 * Hardware (Nucleo-H753ZI MB1364 / U575ZI-Q MB1549 / H563ZI MB1404 /
 *           F446ZE MB1137):
 *
 *   DAC CH1  → PA4 (analog out)  — CN7 pin 17  (D24)   [H7xx, U575, F446]
 *   DAC CH2  → PA5 (analog out)  — CN7 pin 10  (D13)   [H563 - see below]
 *   ADC1 CH10 → PC0 (analog in) — CN9 pin 3   (A1)     [H7xx, H563, F446: CH10=PC0]
 *   ADC1 CH1  → PC0 (analog in) — CN9 pin 11  (A5)     [U575: CH1=PC0, HW verified]
 *
 *   JUMPER REQUIRED:
 *     H7xx : CN7 pin 17  ──── CN9 pin 3   (A1)     (PA4 DAC → PC0 ADC)
 *     U575 : CN7 pin 17  ──── CN9 pin 11  (A5)     (PA4 DAC → PC0 ADC)
 *     H563 : CN7 pin 10  ──── CN9 pin 3   (A1)     (PA5 DAC → PC0 ADC)
 *     F446 : CN7 pin 17  ──── CN9 pin 3   (A1)     (PA4 DAC → PC0 ADC)
 *
 *   The F446 takes the same wiring as the H7xx and falls into the #else
 *   branch below - HW verified 25/08, 9/9 steps within 5%. The only board
 *   that differs on the ADC side is the U575 (CN9-11), and the only one that
 *   differs on the DAC side is the H563 (PA5).
 *
 *   Why the H563 uses DAC channel 2: this part has only two DAC outputs, PA4
 *   and PA5, and on THIS board PA4 reaches only the CN11 morpho header - CN7-17
 *   is PG10/SPI1_NSS here, not PA4 as on the H753. PA5 is D13 on CN7-10, so
 *   channel 2 is the only one a user can actually wire. The datasheet says
 *   which pins can carry the signal; the board manual says which are reachable.
 *
 *   Run "wire" before trusting a reading: it drops both pins to plain GPIO and
 *   tests continuity, which separates a missing jumper from a DAC/ADC problem.
 *
 * The DAC generates a 0-3.3V voltage from the user (0-4095 raw).
 * The ADC reads the same voltage on the connected pin.
 *
 * ADC Channel mapping:
 *   H7xx: Channel 10 = PC0 = ADC12_INP10
 *   U575: Channel  1 = PC0 = ADC12_INP1  (different mapping from H7)
 *
 * CLI Commands (driver):
 *   dac set 1 <0-4095>       — set DAC CH1
 *   dac info                 — show active DAC channels
 *   adc read 1 <ch>          — single polling read
 *   adc volt 1 <ch>          — read in volts
 *   adc isr 1 <ch>           — start continuous ISR read
 *   adc stop 1               — stop ISR
 *
 * STDOUT Log (every 5 s):
 *   EX27: adc=NNNN  tick=NNNNN
 */

#include "caos.h"

#if EN_DAC && EN_ADC

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */

#define ADC_NUM           1U        /* ADC1 */

/* ============================================================================
 * PER-MCU MAPPING — DAC channel, ADC channel and jumper description
 *
 * What changes per board: which DAC output reaches a connector, and which ADC
 * channel number PC0 answers to. Both are BOARD facts, not part facts: the
 * datasheet says which pins CAN carry the signal, the board manual says which
 * ones are wired to a header the user can plug a jumper into.
 * ========================================================================== */

#if MCU_ID == 575
#define DAC_CHANNEL       1U        /* DAC1_OUT1 → PA4, CN7-17 */
#define ADC_CHANNEL       1U        /* CH1  → PC0 = ADC12_INP1  (U575) */
#define DAC_TXT           "DAC  CH1  -> PA4  CN7-17 (D24)"
#define ADC_TXT           "ADC1 CH1  <- PC0  CN9-11 (A5)"
#define WIRE_TXT          "PA4 CN7-17 (DAC out) ---- PC0 CN9-11 (ADC in)"
#define WIRE_OUT_PIN      CA_GPIO_PIN_4

#elif MCU_ID == 563
/* NUCLEO-H563ZI (MB1404): DAC CHANNEL 2, not 1. DAC1_OUT1 is PA4 and on this
 * board PA4 reaches ONLY the CN11 morpho header (UM3115 Table 25, pin 32) -
 * CN7-17 here is D24 = PG10/SPI1_NSS, not PA4 as on the H753. DAC1_OUT2 is
 * PA5, brought out as D13 on CN7-10, so channel 2 is the one a user can wire.
 * PC0 is CN9-3 (A1) = ADC12_INP10, same channel as the H7xx (DS14258 Tab.14). */
#define DAC_CHANNEL       2U        /* DAC1_OUT2 → PA5, CN7-10 */
#define ADC_CHANNEL       10U       /* CH10 → PC0 = ADC12_INP10 */
#define DAC_TXT           "DAC  CH2  -> PA5  CN7-10 (D13)"
#define ADC_TXT           "ADC1 CH10 <- PC0  CN9-3  (A1)"
#define WIRE_TXT          "PA5 CN7-10 (DAC out) ---- PC0 CN9-3  (ADC in)"
#define WIRE_OUT_PIN      CA_GPIO_PIN_5

#else
#define DAC_CHANNEL       1U        /* DAC1_OUT1 → PA4, CN7-17 */
#define ADC_CHANNEL       10U       /* CH10 → PC0 = ADC12_INP10 (H7xx) */
#define DAC_TXT           "DAC  CH1  -> PA4  CN7-17 (D24)"
#define ADC_TXT           "ADC1 CH10 <- PC0  CN9-3  (A1)"
#define WIRE_TXT          "PA4 CN7-17 (DAC out) ---- PC0 CN9-3  (ADC in)"
#define WIRE_OUT_PIN      CA_GPIO_PIN_4
#endif

/* GPIO behind those two analog pins, used by the digital "wire" test */
#define WIRE_OUT_PORT     CA_GPIO_PORT_A   /* PA4 or PA5 — see the block above */
#define WIRE_IN_PORT      CA_GPIO_PORT_C   /* PC0 — ADC input */
#define WIRE_IN_PIN       CA_GPIO_PIN_0
#define ADC_SAMPLE_TIME   4U        /* 64.5 cycles — good accuracy compromise */
#define VREF_MV           3300U     /* 3.3 V reference */


/* ============================================================================
 * MONITOR TASK
 * ========================================================================== */

static u32 task_monitor(void *arg)
{
    (void)arg;

    ca_os_sleep(2000);
    ca_stdout_put_n("EX27: " DAC_TXT);
    ca_stdout_put_n("      " ADC_TXT);
    ca_stdout_put_n("Jumper: " WIRE_TXT);
    ca_stdout_put_n("Commands: test | info | wire   (+ the driver's dac/adc)");

    while (1) {
        ca_os_sleep(5000);
        u16 adc = ca_adc_read(ADC_NUM, ADC_CHANNEL);
        u32 mv  = ((u32)adc * VREF_MV) / 4095U;
        ca_stdout_printf_n("EX27: adc=%u  %u.%03uV  tick=%u",
            (unsigned)adc, mv/1000U, mv%1000U, (unsigned)ca_os_tick_ms());
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
        /* The commands are registered bare - "test", "info", "wire" (see the
         * strncmp chain below) - so the help must name them bare too. It read
         * "ex27 : test | info | wire", which promises a prefix that does not
         * exist: typing what the help showed answered "huh!". Same shape the
         * other examples use ("hello | status | show | wire" in ex21). */
#if MCU_ID == 575
        CLI_PUT_N("test (sweep DAC->ADC) | info | wire | adcmap (CH15-17 pins, no wire)");
#else
        CLI_PUT_N("test (sweep DAC->ADC) | info | wire");
#endif
        return false;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PUT_N("--- Example 27: DAC out + ADC readback ---");
        CLI_PUT_N("  jumper : " WIRE_TXT);
        CLI_PUT_N("  dac    : " DAC_TXT);
        CLI_PUT_N("  adc    : " ADC_TXT);
        u16 raw = ca_adc_read(ADC_NUM, ADC_CHANNEL);
        u32 mv  = ((u32)raw * VREF_MV) / 4095U;
        CLI_PRINTF_N("  reading: raw=%u  %u.%03uV", (u32)raw, mv / 1000U, mv % 1000U);
        return true;
    }
    if (ca_strncmp(p, "test", 4) == 0) {
        /* DAC sweep with ADC readback. Both peripherals are 12-bit over the
         * same VREF, so with the jumper in place the ADC should read back
         * whatever the DAC was told to output, within tolerance.
         * Tolerance is 5% OF FULL SCALE (205 counts), not 5% of the reading:
         * near zero the relative error explodes on a couple of LSB of offset
         * and every low step would be reported as a failure. */
        static const u16 TOL = (4095U * 5U) / 100U;   /* 205 counts */
        u32 steps = 0, bad = 0;
        u16 worst_dev = 0, worst_set = 0, worst_got = 0;

        CLI_PUT_N("--- EX27 test: DAC sweep -> ADC readback ---");
        CLI_PUT_N("  " WIRE_TXT);
        CLI_PRINTF_N("  tolerance: +-%u count (5%% full scale)", (u32)TOL);
        CLI_PUT_N("   set    read     delta   mV      result");

        for (u32 v = 0; v <= 4000U; v += 500U) {
            u16 set = (u16)v;
            ca_dac_write(DAC_CHANNEL, set);
            ca_os_sleep(50);                     /* DAC settling + ADC ready */

            /* ONE conversion per step, taken at face value. This is also a test
             * of the driver itself: ca_adc_hw_read() already waits out the
             * previous conversion and lets the PCSEL analog switch settle, so a
             * single read must be right the first time. The step used to be read
             * twice and only the second value kept — a workaround from when the
             * driver returned the previous channel's residue. If a step starts
             * failing here, the settling belongs back in the driver, not in the
             * example: averaging is not the answer either, since on U5 every
             * ca_adc_read() rewrites PCSEL and reopens the switch, so the
             * samples that follow immediately are garbage (the mean sat at a
             * fixed 1421 whatever the DAC was doing). */
            u16 got = ca_adc_read(ADC_NUM, ADC_CHANNEL);

            u16 dev = (got > set) ? (u16)(got - set) : (u16)(set - got);
            bool ok = (dev <= TOL);
            u32 mv  = ((u32)got * VREF_MV) / 4095U;

            CLI_PRINTF_N("  %4u    %4u    %4u   %u.%03u   %s",
                         (u32)set, (u32)got, (u32)dev,
                         mv / 1000U, mv % 1000U, ok ? "OK" : "FAIL");
            if (!ok) bad++;
            if (dev > worst_dev) { worst_dev = dev; worst_set = set; worst_got = got; }
            steps++;
        }

        ca_dac_write(DAC_CHANNEL, 0U);           /* leave the output at rest */

        CLI_PUT_N("  ------------------------------------------------");
        CLI_PRINTF_N("  max deviation: %u count (set=%u read=%u)",
                     (u32)worst_dev, (u32)worst_set, (u32)worst_got);
        if (bad == 0U)
            CLI_PRINTF_N("  RESULT: PASS  (%u/%u steps within 5%%)", steps, steps);
        else
            CLI_PRINTF_N("  RESULT: ERROR (%u/%u steps out of tolerance) - jumper? try 'wire'",
                         bad, steps);
        return true;
    }
#if MCU_ID == 575
    if (ca_strncmp(p, "adcmap", 6) == 0) {
        /* ADC1 IN13..IN17 -> pin, from the DS13737 pin table. No wire: the pin is
         * driven as a digital output, then ca_adc_pin_init() turns it analog and
         * ca_adc_read() samples the charge still on the floating pad. Only the right pin moves
         * the reading, and the driver must leave THAT pin in analog mode.
         * CH13/14 (PC4/PC5) are left out: those pads are on no Nucleo connector
         * (UM2861 morpho tables) and held no charge on the bench (26/10). */
        static const struct { u8 ch; ca_gpio_port_t port; u8 pin; char name[4]; } MAP[] = {
            { 15U, CA_GPIO_PORT_B, CA_GPIO_PIN_0, "PB0" },
            { 16U, CA_GPIO_PORT_B, CA_GPIO_PIN_1, "PB1" },
            { 17U, CA_GPIO_PORT_B, CA_GPIO_PIN_2, "PB2" },
        };
        u32 bad = 0;
        for (u32 i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
            u16 r[2];
            ca_gpio_init_port(MAP[i].port);
            for (u32 lvl = 0; lvl < 2U; lvl++) {
                ca_gpio_configure(MAP[i].port, MAP[i].pin, CA_GPIO_MODE_OUTPUT,
                                  CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
                ca_gpio_write(MAP[i].port, MAP[i].pin, (u8)lvl);
                ca_os_sleep(1);
                ca_adc_pin_init(ADC_NUM, MAP[i].ch);
                r[lvl] = ca_adc_read(ADC_NUM, MAP[i].ch);
            }
            const GPIO_TypeDef *g = ca_gpio_get_io_port(MAP[i].port);
            const bool analog = ((g->MODER >> (MAP[i].pin * 2U)) & 3U) == 3U;

            /* Which channel does this pad really reach? Same charge trick, the
             * pad made analog by hand, every channel 1..19 read for each level. */
            u16 s[2][20];
            for (u32 lvl = 0; lvl < 2U; lvl++) {
                ca_gpio_configure(MAP[i].port, MAP[i].pin, CA_GPIO_MODE_OUTPUT,
                                  CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
                ca_gpio_write(MAP[i].port, MAP[i].pin, (u8)lvl);
                ca_os_sleep(1);
                ca_gpio_configure(MAP[i].port, MAP[i].pin, CA_GPIO_MODE_ANALOG,
                                  CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
                for (u8 ch = 1U; ch <= 19U; ch++) s[lvl][ch] = ca_adc_read(ADC_NUM, ch);
            }
            u8 best = 0U;
            i32 best_d = 0;
            for (u8 ch = 1U; ch <= 19U; ch++) {
                const i32 d = (i32)s[1][ch] - (i32)s[0][ch];
                if (d > best_d) { best_d = d; best = ch; }
            }

            const bool ok = analog && r[1] > r[0] + 1000U && best == MAP[i].ch;
            if (!ok) bad++;
            CLI_PRINTF_N("  CH%u %s  low %4u  high %4u  pin %s  scan: CH%u +%d  %s",
                         (u32)MAP[i].ch, MAP[i].name, (u32)r[0], (u32)r[1],
                         analog ? "analog" : "NOT analog", (u32)best, best_d,
                         ok ? "OK" : "FAIL");
        }
        CLI_PRINTF_N("  ADCMAP RESULT: %s", bad == 0U ? "PASS" : "FAIL");
        return true;
    }
#endif
    if (ca_strncmp(p, "wire", 4) == 0) {
        /* DIGITAL continuity test of the jumper: PA4 and PC0 are analog here,
         * so they are temporarily reconfigured as plain GPIO — push-pull out
         * and input — and driven low/high. This answers "is the wire there"
         * without depending on DAC or ADC being correct.
         * The two pins do NOT go back to analog afterwards: reset the board
         * to resume the DAC/ADC measurement. */
        /* Park BOTH peripherals before touching the pins. Writing 0 to the DAC
         * is not enough: while the channel is enabled its output buffer stays
         * connected to PA4 and fights the GPIO. The ADC likewise keeps the
         * analog switch in the PC0 pad closed while ADEN is set. */
        ca_dac_stop(DAC_CHANNEL);
        ca_adc_stop(ADC_NUM);

        /* ca_gpio_configure() does NOT enable the port clock — ca_gpio_init_port()
         * does, and this example never called it (the DAC/ADC drivers only set
         * up their own pins). Without this the writes land nowhere and the test
         * reports a missing jumper on a good one. */
        ca_gpio_init_port(WIRE_OUT_PORT);
        ca_gpio_init_port(WIRE_IN_PORT);

        ca_gpio_configure(WIRE_OUT_PORT, WIRE_OUT_PIN, CA_GPIO_MODE_OUTPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
        ca_gpio_configure(WIRE_IN_PORT, WIRE_IN_PIN, CA_GPIO_MODE_INPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

        ca_gpio_write(WIRE_OUT_PORT, WIRE_OUT_PIN, 0);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 lo = ca_gpio_read(WIRE_IN_PORT, WIRE_IN_PIN);
        ca_gpio_write(WIRE_OUT_PORT, WIRE_OUT_PIN, 1);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 hi = ca_gpio_read(WIRE_IN_PORT, WIRE_IN_PIN);

        CLI_PUT_N("  " WIRE_TXT);
        CLI_PRINTF_N("  drive0->read%u  drive1->read%u  %s", lo, hi,
                     (lo == 0 && hi == 1) ? "OK (jumper present)"
                                          : "FAIL (jumper missing or wrong)");
        /* One line the bench can read, identical in every example that has a
         * `wire` command - see the note in ex64. */
        CLI_PRINTF_N("  WIRE RESULT : %s  (%u/1)",
                     (lo == 0 && hi == 1) ? "OK" : "FAIL",
                     (u32)((lo == 0 && hi == 1) ? 1U : 0U));
        /* DAC and ADC are now stopped and the pins are plain GPIO, so there is
         * no way back to the analog measurement from here: reboot instead of
         * asking the user to press the button. */
        CLI_PUT_N("  rebooting in 2.5 s to restore the analog measurement...");
        ca_os_sleep(2500);         /* leave the result on screen, then reboot */
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
    /* DAC CH1: PA4 analog output */
    ca_dac_config_t dac_cfg = { DAC_CHANNEL };
    ca_dac_init(&dac_cfg);
    ca_dac_write(DAC_CHANNEL, 0U);

    /* ADC CH10: PC0 analog input */
    ca_adc_config_t adc_cfg = { ADC_NUM, ADC_SAMPLE_TIME };
    ca_adc_init(&adc_cfg);
    ca_adc_pin_init(ADC_NUM, ADC_CHANNEL);


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

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}

#else  /* EN_DAC && EN_ADC */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m27a, "=== Example 27 ===");
PRIVATE_STRING(m27b, "EN_DAC / EN_ADC ==0, skipping DAC/ADC example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m27a);
    PUT_N(&priv_m27b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_DAC && EN_ADC */
