// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex99.c
 * @brief          : Example 99 — OPAMP follower and PGA, inside the chip (U575)
 ******************************************************************************
 * @details
 * DAC CH1 -> OPAMP1 (internal input) -> ADC1, no wire: the OPAMP output is
 * also an ADC channel (ch8, PA3), and the ADC reads the DAC pin too (ch9,
 * PA4), so the input is MEASURED, not assumed from the code written to the
 * DAC: the DAC's own errors drop out of the verdict.
 *
 *   opamp test   follower: out - in within 20 counts (DS13737 offset +-7 mV
 *                = 9 counts, plus ADC noise), at five levels;
 *                PGA x2/x4/x8/x16: gain from two points (offset cancels)
 *                within 3% (DS13737: +-1%, plus the ADC on a small input).
 *                Ends with "OPA TEST PASS" or "OPA TEST FAIL".
 *
 * H753: OPAMP1's output is PC4, RMII_RXD0 on the Nucleo (SB36 ON) - it would
 * fight the Ethernet PHY - and OPAMP2's output reaches no ADC. The example
 * says so and stops; the driver is there, untested on that board.
 ******************************************************************************
 */

#include "caos.h"

#if EN_DAC && EN_ADC && MCU_ID == 575

#define OPA_NUM        1U
#define DAC_CH         1U       /* DAC1_OUT1 -> OPAMP1 VINP (internal), PA4 */
#define ADC_NUM        1U
#define ADC_CH_IN      9U       /* PA4 = the DAC pin */
#define ADC_SAMPLE     4U       /* as ex27 */
#define READS          8U

#define FOLLOW_TOL     20       /* counts */
#define GAIN_TOL_PCT   3U

/* Mean of READS conversions, spaced 1 ms: back-to-back reads on U5 reopen
 * the PCSEL switch and the next samples are not settled (see ex27). */
static u32 opa_read(u8 ch)
{
    u32 sum = 0U;
    for (u32 i = 0U; i < READS; i++) {
        sum += ca_adc_read(ADC_NUM, ch);
        ca_os_sleep(1);
    }
    return (sum + READS / 2U) / READS;
}

static void opa_set(u16 dac)
{
    ca_dac_write(DAC_CH, dac);
    ca_os_sleep(2);
}

static bool opa_follower(u8 ch_out)
{
    static const u16 levels[] = { 300U, 1000U, 2000U, 3000U, 3700U };
    bool ok = true;
    (void)ca_opamp_init(OPA_NUM, CA_OPAMP_IN_DAC, 1U);
    for (u32 i = 0U; i < sizeof(levels) / sizeof(levels[0]); i++) {
        opa_set(levels[i]);
        const i32 in  = (i32)opa_read(ADC_CH_IN);
        const i32 out = (i32)opa_read(ch_out);
        const i32 d   = out - in;
        const bool good = d >= -FOLLOW_TOL && d <= FOLLOW_TOL;
        ok = ok && good;
        CLI_PRINTF_N("OPA x1  dac %4u  in %4d  out %4d  diff %d %s", (u32)levels[i], in, out, d,
                     good ? "" : "<-- FAIL");
    }
    return ok;
}

/* Two inputs that keep the output between ~15% and ~85% of full scale:
 * the slope between them is the gain, with the offset cancelled. */
static bool opa_pga(u8 ch_out, u32 gain)
{
    (void)ca_opamp_init(OPA_NUM, CA_OPAMP_IN_DAC, gain);
    const u16 lo = (u16)(600U / gain), hi = (u16)(3400U / gain);
    opa_set(lo);
    const i32 in1 = (i32)opa_read(ADC_CH_IN), out1 = (i32)opa_read(ch_out);
    opa_set(hi);
    const i32 in2 = (i32)opa_read(ADC_CH_IN), out2 = (i32)opa_read(ch_out);
    if (in2 <= in1) {
        CLI_PRINTF_N("OPA x%-2u input did not move (%d -> %d) <-- FAIL", gain, in1, in2);
        return false;
    }
    /* gain x 1000 */
    const i32 g   = ((out2 - out1) * 1000) / (in2 - in1);
    const i32 dev = ((g - (i32)gain * 1000) * 100) / ((i32)gain * 1000);   /* % */
    const bool ok = dev >= -(i32)GAIN_TOL_PCT && dev <= (i32)GAIN_TOL_PCT;
    CLI_PRINTF_N("OPA x%-2u in %d..%d  out %d..%d  gain %d.%03d (%d%%) %s", gain, in1, in2,
                 out1, out2, g / 1000, g % 1000, dev, ok ? "" : "<-- FAIL");
    return ok;
}

static void opa_test(void)
{
    u8 adc = 0U;
    const u8 ch_out = ca_opamp_adc_channel(OPA_NUM, &adc);
    ca_adc_pin_init(ADC_NUM, ADC_CH_IN);

    bool ok = opa_follower(ch_out);
    ok = opa_pga(ch_out, 2U)  && ok;
    ok = opa_pga(ch_out, 4U)  && ok;
    ok = opa_pga(ch_out, 8U)  && ok;
    ok = opa_pga(ch_out, 16U) && ok;

    ca_opamp_stop(OPA_NUM);
    ca_dac_write(DAC_CH, 0U);
    CLI_PUT_N(ok ? "OPA TEST PASS" : "OPA TEST FAIL");
}

#if ENABLE_CLI
static bool ex99_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("opamp: test"); return false; }
    if (ca_strncmp(p, "opamp", 5) != 0) return false;
    p = ca_skip_sp(p + 5);
    if (ca_strncmp(p, "test", 4) == 0) { opa_test(); return true; }
    CLI_PUT_N("opamp: test");
    return true;
}
static ca_cli_node_t s_cli = { ex99_cli, NULL };
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
        ca_cli_register(&s_cli);
    )
}

void ca_os_main(void)
{
    ca_dac_config_t dac_cfg = { DAC_CH };
    (void)ca_dac_init(&dac_cfg);
    ca_dac_write(DAC_CH, 0U);
    ca_adc_config_t adc_cfg = { ADC_NUM, ADC_SAMPLE };
    (void)ca_adc_init(&adc_cfg);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m99a, "=== Example 99 ===");
#if MCU_ID == 753
PRIVATE_STRING(m99b, "ex99: OPAMP1 out is PC4 = RMII_RXD0 (SB36), OPAMP2 out reaches no ADC");
#elif !(EN_DAC && EN_ADC)
PRIVATE_STRING(m99b, "ex99: EN_DAC / EN_ADC disabled");
#else
PRIVATE_STRING(m99b, "ex99: no OPAMP on this part (U575 and H753 have them)");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m99a);
    PUT_N(&priv_m99b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
