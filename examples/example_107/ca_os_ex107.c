// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex107.c
 * @brief          : Example 107 — comparator with a DAC threshold (U575, H753)
 ******************************************************************************
 * @details
 * COMP1 compares PB2 with DAC channel 1 inside the chip (dac.h,
 * ca_dac_comp_*). DAC channel 2 makes the signal, one jumper the same on
 * both Nucleo:
 *
 *     PA5 DAC CH2  CN7-10  ---->  PB2 COMP1_INP2  CN9-13 (U575)
 *                                        CN12-22, morpho (H753: SB69 open,
 *                                        CN9-13 is not PB2 on the bench board)
 *
 *   comp test [thr]   CH1 = threshold; CH2 climbs 2 LSB at a time until the
 *                     comparator trips (16 reads agreeing), then falls until
 *                     it returns, for the four hysteresis settings. Pass:
 *                     - every midpoint on the threshold within 40 LSB
 *                       (~32 mV: the COMP's 20 mV offset and the DACs');
 *                     - 'high' wider than 'none' by 8 LSB (6 mV) or more;
 *                     - 'medium' no wider than 'high' (+1 step).
 *                     The datasheet gives the widths as typical only (0/10/
 *                     20/30 mV): 'none' and 'low' are printed, not judged.
 *                     H753 measured 'low' 2-6 LSB NEGATIVE, repeatably, and
 *                     every setting moves the rising trip ~10 mV down.
 *   comp all          thresholds 1024, 2048, 3072: "COMP ALL PASS (3/3)"
 *   wire              the jumper as plain GPIO, then reboot
 *
 * 1 LSB = 3.3 V / 4096 = 0.81 mV with VREF+ = VDDA on the Nucleo.
 ******************************************************************************
 */

#include "caos.h"

#if EN_DAC

#define STEP        2U          /* LSB per step of the signal */
#define SPAN        300U        /* search window around the threshold, LSB */
#define MID_TOL     40U         /* midpoint within 40 LSB of the threshold */
#define SETTLE_US   30U         /* DAC settling + comparator delay, generous */
#define READS       16U         /* a trip counts when this many reads agree */

/* The line the bench shows the operator: the position of PB2 on this board */
#if MCU_ID == 753
#define WIRE_TXT    "PA5 DAC CH2 CN7-10 ---- PB2 COMP1_INP2 CN12-22 (morpho)"
#else
#define WIRE_TXT    "PA5 DAC CH2 CN7-10 ---- PB2 COMP1_INP2 CN9-13"
#endif
#define HYST_MIN    8           /* high - none, LSB */

static const char *const s_hyst_name[] = { "none", "low", "medium", "high" };

static void ex107_wait_us(u32 us)
{
    const u32 cyc = (SystemCoreClock / 1000000U) * us;
    const u32 t0 = DWT->CYCCNT;
    while ((DWT->CYCCNT - t0) < cyc) { }
}

/* The output is `level` on every one of READS reads, 2 us apart */
static bool ex107_settled(bool level)
{
    for (u32 i = 0U; i < READS; i++) {
        if (ca_dac_comp_level() != level) return false;
        ex107_wait_us(2U);
    }
    return true;
}

/* Signal value at which the output has changed to `level` for good,
 * walking from `from` toward `to`; 0xFFFF = never. A single read took the
 * first noise spike: with the H753's 'low' hysteresis the rise came 6 LSB
 * under the fall, a negative width. */
static u32 ex107_trip(u32 from, u32 to, bool level)
{
    const bool up = to > from;
    for (u32 v = from; up ? v <= to : v >= to; v = up ? v + STEP : v - STEP) {
        ca_dac_write(2U, (u16)v);
        ex107_wait_us(SETTLE_US);
        if (ex107_settled(level)) return v;
        if (!up && v < STEP) break;
    }
    return 0xFFFFU;
}

typedef struct { u32 rise, fall; } trips_t;

static bool ex107_threshold(u32 thr, bool verbose)
{
    const u32 lo = (thr > SPAN) ? thr - SPAN : 0U;
    const u32 hi = (thr + SPAN < 4095U) ? thr + SPAN : 4095U;
    trips_t t[4];
    bool ok = true;

    ca_dac_write(1U, (u16)thr);
    for (u32 h = 0U; h < 4U; h++) {
        ca_dac_comp_init(1U, (ca_dac_comp_hyst_t)h);
        /* Start well below: the output must be low there */
        ca_dac_write(2U, (u16)lo);
        ex107_wait_us(200U);
        const bool low_at_start = !ca_dac_comp_level();
        t[h].rise = ex107_trip(lo, hi, true);
        t[h].fall = (t[h].rise == 0xFFFFU) ? 0xFFFFU : ex107_trip(hi, lo, false);
        ca_dac_write(2U, (u16)lo);

        const bool found = low_at_start && t[h].rise != 0xFFFFU && t[h].fall != 0xFFFFU;
        const u32 mid = found ? (t[h].rise + t[h].fall) / 2U : 0U;
        const u32 off = (mid > thr) ? mid - thr : thr - mid;
        const i32 width = (i32)t[h].rise - (i32)t[h].fall;
        const bool good = found && off <= MID_TOL;
        if (!found) t[h].rise = t[h].fall = 0U;
        if (verbose || !good) {
            if (!found)
                CLI_PRINTF_N("CMP thr %4u hyst %-6s  no trip (start %s)  BAD", thr, s_hyst_name[h],
                             low_at_start ? "low" : "HIGH");
            else
                CLI_PRINTF_N("CMP thr %4u hyst %-6s  rise %4u fall %4u  width %s%u LSB (%u mV)  mid %s%u  %s",
                             thr, s_hyst_name[h], t[h].rise, t[h].fall,
                             (width < 0) ? "-" : "", (u32)((width < 0) ? -width : width),
                             (u32)((width < 0) ? 0 : width) * 3300U / 4096U,
                             (mid < thr) ? "-" : "+", off, good ? "ok" : "BAD");
        }
        if (!good) ok = false;
    }

    /* The hysteresis works: 'high' clearly wider than 'none', 'medium' not
     * wider than 'high' */
    const i32 w_none = (i32)t[0].rise - (i32)t[0].fall;
    const i32 w_med  = (i32)t[2].rise - (i32)t[2].fall;
    const i32 w_high = (i32)t[3].rise - (i32)t[3].fall;
    const bool hyst_ok = w_high - w_none >= HYST_MIN && w_med <= w_high + (i32)STEP;
    if (verbose || !hyst_ok)
        CLI_PRINTF_N("CMP thr %4u hysteresis high-none %s%u LSB (min %u), medium<=high %s  %s", thr,
                     (w_high < w_none) ? "-" : "", (u32)((w_high < w_none) ? w_none - w_high : w_high - w_none),
                     (u32)HYST_MIN, (w_med <= w_high + (i32)STEP) ? "yes" : "NO", hyst_ok ? "ok" : "BAD");
    if (!hyst_ok) ok = false;
    ca_dac_comp_stop();
    return ok;
}

static bool ex107_setup(void)
{
    const ca_dac_config_t c1 = { 1U }, c2 = { 2U };
    return ca_dac_init(&c1) && ca_dac_init(&c2) && ca_dac_comp_init(1U, CA_DAC_COMP_HYST_NONE);
}

static void ex107_test(u32 thr)
{
    const bool ok = ex107_threshold(thr, true);
    CLI_PUT_N(ok ? "COMP TEST PASS" : "COMP TEST FAIL");
}

static void ex107_all(void)
{
    static const u32 thr[] = { 1024U, 2048U, 3072U };
    u32 good = 0U;
    for (u32 i = 0U; i < 3U; i++) if (ex107_threshold(thr[i], true)) good++;
    CLI_PRINTF_N("COMP ALL %s  (%u/3)", (good == 3U) ? "PASS" : "FAIL", good);
}

/* DIGITAL continuity of the jumper: the DAC and the comparator are stopped,
 * the pins become plain GPIO, and the board reboots */
static void ex107_wire(void)
{
    ca_dac_comp_stop();
    ca_dac_stop(2U);
    ca_gpio_init_port(CA_GPIO_PORT_A);
    ca_gpio_init_port(CA_GPIO_PORT_B);
    ca_gpio_configure(CA_GPIO_PORT_A, CA_GPIO_PIN_5, CA_GPIO_MODE_OUTPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_configure(CA_GPIO_PORT_B, CA_GPIO_PIN_2, CA_GPIO_MODE_INPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

    ca_gpio_write(CA_GPIO_PORT_A, CA_GPIO_PIN_5, 0);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 lo = ca_gpio_read(CA_GPIO_PORT_B, CA_GPIO_PIN_2);
    ca_gpio_write(CA_GPIO_PORT_A, CA_GPIO_PIN_5, 1);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 hi = ca_gpio_read(CA_GPIO_PORT_B, CA_GPIO_PIN_2);
    const bool ok = (lo == 0U && hi == 1U);

    CLI_PUT_N("  " WIRE_TXT);
    CLI_PRINTF_N("  drive0->read%u  drive1->read%u  %s", lo, hi,
                 ok ? "OK (jumper present)" : "FAIL (jumper missing or wrong)");
    /* The line the bench reads, identical in every example with `wire` */
    CLI_PRINTF_N("  WIRE RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
    CLI_PUT_N("  rebooting in 2.5 s to give the pins back to the DAC...");
    ca_os_sleep(2500);
    ca_os_reset();
}

#if ENABLE_CLI
static void ex107_usage(void)
{
    CLI_PUT_N("comp      : test [threshold 0-4095] | all");
    CLI_PUT_N("wire      : DAC CH2 -> COMP1 input jumper continuity as plain GPIO, then reboot");
}

static bool ex107_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { ex107_usage(); return false; }
    if (ca_strncmp(p, "wire", 4) == 0) { ex107_wire(); return true; }
    if (ca_strncmp(p, "comp", 4) != 0) return false;
    p = ca_skip_sp(p + 4);

    if (ca_strncmp(p, "all", 3) == 0) { ex107_all(); return true; }
    if (ca_strncmp(p, "test", 4) == 0) {
        p = ca_skip_sp(p + 4);
        u32 thr = 2048U;
        if (*p >= '0' && *p <= '9') thr = (u32)ca_strtol(p, 10);
        if (thr < SPAN || thr > 4095U - SPAN) { CLI_PRINTF_N("threshold %u..%u", SPAN, 4095U - SPAN); return true; }
        ex107_test(thr);
        return true;
    }
    ex107_usage();
    return true;
}
static ca_cli_node_t s_cli = { ex107_cli, NULL };
#endif

PRIVATE_STRING(why, "ex107: this part has no comparator with a DAC input (U575 and H753 have it)");

void ca_os_configure(void)
{
    if (!ex107_setup()) {
        /* The line the bench matches to know this example cannot run on
           this part. The reason follows it. */
        CA_EXAMPLE_UNSUPPORTED();
        PUT_N(&priv_why);
        return;
    }
    ca_dac_comp_stop();

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

#else  /* EN_DAC */

PRIVATE_STRING(why, "ex107: built without EN_DAC");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_DAC */
