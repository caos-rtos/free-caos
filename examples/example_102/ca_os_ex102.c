// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex102.c
 * @brief          : Example 102 — FMAC filters on a DAC -> ADC chain (H563, U575)
 ******************************************************************************
 * @details
 * Two halves, like ex98 for the CORDIC:
 *
 *   fmac test     no wiring needed. Six filters (FIR up to 127 taps, IIR,
 *                 output gain, clip on and off) run on the FMAC over a noise
 *                 input fed in uneven blocks, and the result is compared word
 *                 for word with four C models of the arithmetic (each product
 *                 truncated to the q4.22 accumulator or kept whole; output
 *                 truncated or rounded). The model that matches every sample
 *                 is the FMAC arithmetic. Then the cost per sample against
 *                 plain C. Ends with "FMC TEST PASS" or "FMC TEST FAIL".
 *   fmac dump     the same cases as "FMC" lines for fmac_check.py on the PC,
 *                 which recomputes them with numpy, independently of this file.
 *
 * and a measuring bench on a real analog channel, through a jumper:
 *
 *     U575 : PA4 CN7-17 (DAC1_OUT1) ---- PC0 CN9-11 (ADC1 CH1)
 *     H563 : PA5 CN7-10 (DAC1_OUT2) ---- PC0 CN9-3  (ADC1 CH10)
 *
 * The H563 ADC has no internal route from the DAC (RM0481 table 257: only
 * temperature, VREFINT, VBAT/4, VDDCORE), so both boards use the jumper.
 *
 *   fmac filter [name]       select the filter (lp1k bp1k hp100 lp1k_iir)
 *   fmac tone <hz>           one tone through the chain: level, SNR and THD
 *                            of DAC+ADC, then level and gain after the filter
 *   fmac sweep [f0 f1 step]  the filter's frequency response, measured
 *
 * The filters are designed for fs = 16 kHz. In a DAC -> ADC loopback the ADC
 * reads the level the DAC is holding, so the time between two samples does not
 * enter the measurement: samples are taken one after the other in a task and
 * fs is the design rate of the filters, not a clock. A real sample clock (and
 * its jitter) enters with I2S; the measuring part below is kept apart from
 * the FMAC so it can be pointed at that channel.
 *
 * Tones are snapped to an FFT bin (fs/N = 15.625 Hz) so N samples hold a whole
 * number of periods: Goertzel then reads each harmonic without a window.
 ******************************************************************************
 */

#include "caos.h"

#if EN_FMAC && EN_DAC && EN_ADC

/* ============================================================================
 * PER-MCU MAPPING — as ex27, the two parts with an FMAC
 * ========================================================================== */

#define ADC_NUM           1U
#define ADC_SAMPLE_TIME   4U        /* as ex27 */

#if MCU_ID == 575
#define DAC_CHANNEL       1U        /* DAC1_OUT1 -> PA4, CN7-17 */
#define ADC_CHANNEL       1U        /* CH1 -> PC0 = ADC12_INP1 */
#define WIRE_TXT          "PA4 CN7-17 (DAC out) ---- PC0 CN9-11 (ADC in)"
#define WIRE_OUT_PIN      CA_GPIO_PIN_4
#else   /* 563 */
#define DAC_CHANNEL       2U        /* DAC1_OUT2 -> PA5, CN7-10 (PA4 is on CN11 only) */
#define ADC_CHANNEL       10U       /* CH10 -> PC0 = ADC12_INP10 */
#define WIRE_TXT          "PA5 CN7-10 (DAC out) ---- PC0 CN9-3  (ADC in)"
#define WIRE_OUT_PIN      CA_GPIO_PIN_5
#endif

/* GPIO behind the two analog pins, for the digital `wire` test (as ex27) */
#define WIRE_OUT_PORT     CA_GPIO_PORT_A
#define WIRE_IN_PORT      CA_GPIO_PORT_C
#define WIRE_IN_PIN       CA_GPIO_PIN_0

/* ============================================================================
 * FILTERS — fs = 16 kHz, q1.15, from scipy (fmac_check.py --tables prints
 * them again):
 *   lp1k      FIR  63 taps, firwin low-pass 1 kHz (Hamming)
 *   bp1k      FIR 127 taps, firwin band-pass 800..1250 Hz: the longest FIR
 *   hp100     IIR biquad, Butterworth high-pass 100 Hz, coefficients / 2, gain 1
 *   lp1k_iir  IIR 4th order, Butterworth low-pass 1 kHz, coefficients / 4, gain 2
 * IIR a[] is -a[1..] of scipy, as the FMAC adds the feedback.
 * ========================================================================== */

static const i16 s_lp1k_b[63] = {
       -10,    -20,    -30,    -38,    -42,    -39,    -26,      0,     39,     87,
       136,    176,    193,    174,    111,      0,   -150,   -322,   -487,   -610,
      -652,   -579,   -365,      0,    508,   1131,   1821,   2517,   3153,   3663,
      3993,   4107,   3993,   3663,   3153,   2517,   1821,   1131,    508,      0,
      -365,   -579,   -652,   -610,   -487,   -322,   -150,      0,    111,    174,
       193,    176,    136,     87,     39,      0,    -26,    -39,    -42,    -38,
       -30,    -20,    -10,
};
static const i16 s_bp1k_b[127] = {
       -17,    -19,    -18,    -14,     -5,      7,     20,     33,     43,     47,
        42,     29,      7,    -20,    -48,    -70,    -82,    -80,    -64,    -37,
        -3,     30,     54,     65,     60,     42,     20,      3,      1,     18,
        54,    101,    145,    166,    147,     78,    -43,   -202,   -369,   -510,
      -584,   -558,   -414,   -158,    181,    548,    876,   1095,   1148,   1001,
       660,    164,   -410,   -965,  -1400,  -1627,  -1592,  -1287,   -752,    -70,
       644,   1268,   1692,   1843,   1692,   1268,    644,    -70,   -752,  -1287,
     -1592,  -1627,  -1400,   -965,   -410,    164,    660,   1001,   1148,   1095,
       876,    548,    181,   -158,   -414,   -558,   -584,   -510,   -369,   -202,
       -43,     78,    147,    166,    145,    101,     54,     18,      1,      3,
        20,     42,     60,     65,     54,     30,     -3,    -37,    -64,    -80,
       -82,    -70,    -48,    -20,      7,     29,     42,     47,     43,     33,
        20,      7,     -5,    -14,    -18,    -19,    -17,
};
static const i16 s_hp100_b[3] = {
     15935, -31871,  15935,
};
static const i16 s_hp100_a[2] = {
     31858, -15499,
};
static const i16 s_lp1k_iir_b[5] = {
         8,     31,     46,     31,      8,
};
static const i16 s_lp1k_iir_a[4] = {
     24386, -28036,  14632,  -2913,
};
typedef struct {
    const char       *name;
    ca_fmac_filter_t  f;
} ex102_filter_t;

/* The IIRs clip. Without it a transient a little above full scale wraps,
 * the wrapped sample comes back through the feedback and the filter never
 * recovers: hp100 fed 0.9 FS at 400..700 Hz overshoots 1.0 by 0.3-1% around
 * sample 20 and then oscillates at full scale, -12 dB at the tone. Saturating
 * keeps the error to the one overshoot. A FIR has no feedback: a wrap there is
 * one wrong sample, not a state. (The bit-exact cases set clip themselves.) */
#define EX102_FIL(n, g)       { #n, { s_##n##_b, sizeof(s_##n##_b) / 2U, NULL, 0U, g, false } }
#define EX102_IIR(n, g)       { #n, { s_##n##_b, sizeof(s_##n##_b) / 2U, \
                                      s_##n##_a, sizeof(s_##n##_a) / 2U, g, true } }

static const ex102_filter_t s_filters[] = {
    EX102_FIL(lp1k, 0U),
    EX102_FIL(bp1k, 0U),
    EX102_IIR(hp100, 1U),
    EX102_IIR(lp1k_iir, 2U),
};
#define EX102_NFIL  (sizeof(s_filters) / sizeof(s_filters[0]))

static u32 s_sel;       /* filter used by tone and sweep */

/* ============================================================================
 * BIT-EXACT TEST
 * ========================================================================== */

#define EX102_TLEN   600U

static i16 s_x[EX102_TLEN];
static i16 s_y[EX102_TLEN];
static i16 s_ref[EX102_TLEN];

/* The cases: filter, gain and clip. lp1k at gain 3 overflows the q1.15 output
 * on purpose, once wrapping and once clipped. */
typedef struct { u32 fil; u32 gain; bool clip; } ex102_case_t;
static const ex102_case_t s_cases[] = {
    { 0U, 0U, false }, { 1U, 0U, false }, { 2U, 1U, false },
    { 3U, 2U, false }, { 0U, 3U, false }, { 0U, 3U, true  },
};
#define EX102_NCASE  (sizeof(s_cases) / sizeof(s_cases[0]))

/* Models: bit 0 = each product truncated to the 22 fractional bits of the
 * accumulator (else kept whole, 30 bits); bit 1 = output rounded (else
 * truncated). The accumulator has 4 integer bits and wraps either way. */
#define EX102_NMODEL 4U
static const char *const s_model_txt[EX102_NMODEL] = {
    "products whole, output truncated", "products truncated, output truncated",
    "products whole, output rounded",   "products truncated, output rounded",
};

static i64 ex102_wrap(i64 v, u32 bits)
{
    const u32 sh = 64U - bits;
    return (i64)((u64)v << sh) >> sh;
}

static void ex102_model(const ca_fmac_filter_t *f, u32 model, const i16 *x, i16 *y, u32 len)
{
    const u32 frac = (model & 1U) ? 22U : 30U;     /* accumulator fraction */
    const u32 drop = (model & 1U) ? 8U : 0U;       /* product bits lost */
    const u32 shift = frac - 15U;                  /* down to q1.15 */

    for (u32 n = 0U; n < len; n++) {
        i64 acc = 0;
        for (u32 k = 0U; k < f->nb && k <= n; k++)
            acc += ((i64)f->b[k] * x[n - k]) >> drop;
        for (u32 k = 1U; k <= f->na && k <= n; k++)
            acc += ((i64)f->a[k - 1U] * y[n - k]) >> drop;
        acc = ex102_wrap(acc, frac + 4U);

        i64 v = acc * ((i64)1 << f->gain);
        if (model & 2U) v += (i64)1 << (shift - 1U);
        v >>= shift;
        if (f->clip) {
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
        }
        y[n] = (i16)(u16)(u64)v;                    /* no clip: low 16 bits */
    }
}

/* Deterministic noise: full-scale LCG, then the IIR cases get half of it */
static void ex102_input(u32 c)
{
    u32 r = 0x1234567U + c;
    const u32 sh = (s_filters[s_cases[c].fil].f.na != 0U) ? 1U : 0U;
    for (u32 i = 0U; i < EX102_TLEN; i++) {
        r = r * 1664525U + 1013904223U;
        s_x[i] = (i16)((i32)(i16)(r >> 16) >> sh);
    }
}

/* FMAC over the input, in uneven blocks: the state must carry across calls */
static bool ex102_run(const ca_fmac_filter_t *f, u32 *flags)
{
    static const u32 blocks[] = { 1U, 7U, 64U, 128U, 200U };
    if (!ca_fmac_start(f)) return false;
    u32 done = 0U, b = 0U;
    while (done < EX102_TLEN) {
        u32 n = (b < 5U) ? blocks[b++] : EX102_TLEN - done;
        if (n > EX102_TLEN - done) n = EX102_TLEN - done;
        if (ca_fmac_run(&s_x[done], &s_y[done], n) != n) { ca_fmac_stop(); return false; }
        done += n;
    }
    *flags = ca_fmac_flags();
    ca_fmac_stop();
    return true;
}

static ca_fmac_filter_t ex102_case_filter(u32 c)
{
    ca_fmac_filter_t f = s_filters[s_cases[c].fil].f;
    f.gain = s_cases[c].gain;
    f.clip = s_cases[c].clip;
    return f;
}

/* Plain C FIR, the cost the FMAC takes off the CPU */
static i16 ex102_cpu_fir(const i16 *b, u32 nb, const i16 *x, u32 n)
{
    i64 acc = 0;
    for (u32 k = 0U; k < nb && k <= n; k++) acc += (i32)b[k] * x[n - k];
    return (i16)(acc >> 15);
}

static void ex102_test(void)
{
    u32 bad[EX102_NMODEL] = { 0U };
    bool ran = true;

    for (u32 c = 0U; c < EX102_NCASE; c++) {
        const ca_fmac_filter_t f = ex102_case_filter(c);
        u32 flags = 0U;
        ex102_input(c);
        if (!ex102_run(&f, &flags)) {
            CLI_PRINTF_N("FMC case %u %s: FMAC stalled", c, s_filters[s_cases[c].fil].name);
            ran = false;
            continue;
        }
        u32 m_bad[EX102_NMODEL];
        for (u32 m = 0U; m < EX102_NMODEL; m++) {
            ex102_model(&f, m, s_x, s_ref, EX102_TLEN);
            m_bad[m] = 0U;
            for (u32 i = 0U; i < EX102_TLEN; i++) if (s_ref[i] != s_y[i]) m_bad[m]++;
            bad[m] += m_bad[m];
        }
        CLI_PRINTF_N("FMC case %u %-8s nb %3u na %u gain %u clip %u  differ %u %u %u %u  flags %02X",
                     c, s_filters[s_cases[c].fil].name, f.nb, f.na, f.gain, (u32)f.clip,
                     m_bad[0], m_bad[1], m_bad[2], m_bad[3], flags);
    }

    u32 match = EX102_NMODEL;
    for (u32 m = 0U; m < EX102_NMODEL; m++) {
        if (bad[m] == 0U) { match = m; break; }
    }
    if (match < EX102_NMODEL)
        CLI_PRINTF_N("FMC arithmetic: %s (model %u, every sample)", s_model_txt[match], match);
    else
        CLI_PUT_N("FMC arithmetic: no model matches every sample");

    /* Cost per sample of the longest FIR: FMAC (polled) against plain C.
     * Timed with interrupts off: the lines printed just above are still
     * draining through the stdout task and the UART, and their interrupts
     * and switches landing in the window made the same filter cost 597 or
     * 261 cycles depending on what was queued (U575, 25/10). */
    const ca_fmac_filter_t *bp = &s_filters[1].f;
    ex102_input(1U);
    u32 flags = 0U;
    ca_enter_critical();
    u32 t0 = DWT->CYCCNT;
    (void)ex102_run(bp, &flags);
    const u32 c_fmac = (DWT->CYCCNT - t0) / EX102_TLEN;
    t0 = DWT->CYCCNT;
    for (u32 i = 0U; i < EX102_TLEN; i++) s_ref[i] = ex102_cpu_fir(bp->b, bp->nb, s_x, i);
    const u32 c_cpu = (DWT->CYCCNT - t0) / EX102_TLEN;
    ca_exit_critical();
    CLI_PRINTF_N("FMC %u-tap FIR: FMAC %u cycles/sample, plain C %u (%u MHz)",
                 bp->nb, c_fmac, c_cpu, SystemCoreClock / 1000000U);

    /* The same cost for each filter */
    for (u32 i = 0U; i < EX102_NFIL; i++) {
        const ca_fmac_filter_t *f = &s_filters[i].f;
        ex102_input(0U);
        ca_enter_critical();
        t0 = DWT->CYCCNT;
        (void)ex102_run(f, &flags);
        const u32 c = (DWT->CYCCNT - t0) / EX102_TLEN;
        ca_exit_critical();
        CLI_PRINTF_N("FMC cost %-8s %3u+%u taps: %u cycles/sample", s_filters[i].name,
                     f->nb, f->na, c);
    }

    CLI_PUT_N((ran && match < EX102_NMODEL) ? "FMC TEST PASS" : "FMC TEST FAIL");
}

static void ex102_dump_words(const char *tag, const i16 *v, u32 n)
{
    static char line[96];
    for (u32 i = 0U; i < n; i += 16U) {
        u32 pos = 0U;
        line[pos++] = 'F'; line[pos++] = 'M'; line[pos++] = 'C'; line[pos++] = ' ';
        line[pos++] = tag[0];
        for (u32 j = i; j < n && j < i + 16U; j++) {
            static const char hx[] = "0123456789ABCDEF";
            const u16 w = (u16)v[j];
            line[pos++] = ' ';
            line[pos++] = hx[(w >> 12) & 0xFU]; line[pos++] = hx[(w >> 8) & 0xFU];
            line[pos++] = hx[(w >> 4) & 0xFU];  line[pos++] = hx[w & 0xFU];
        }
        line[pos] = '\0';
        CLI_PRINTF_N("%s", line);
    }
}

static void ex102_dump(void)
{
    u32 n = 0U;
    for (u32 c = 0U; c < EX102_NCASE; c++) {
        const ca_fmac_filter_t f = ex102_case_filter(c);
        u32 flags = 0U;
        ex102_input(c);
        if (!ex102_run(&f, &flags)) continue;
        CLI_PRINTF_N("FMC F %s %u %u %u %u %u", s_filters[s_cases[c].fil].name,
                     f.nb, f.na, f.gain, (u32)f.clip, EX102_TLEN);
        ex102_dump_words("B", f.b, f.nb);
        if (f.na != 0U) ex102_dump_words("A", f.a, f.na);
        ex102_dump_words("X", s_x, EX102_TLEN);
        ex102_dump_words("Y", s_y, EX102_TLEN);
        n++;
    }
    CLI_PRINTF_N("FMC END %u", n);
}

/* ============================================================================
 * MEASURING — independent of the FMAC: a capture of q1.15 samples and their
 * levels. Only ex102_capture() knows the channel is a DAC and an ADC.
 * ========================================================================== */

#define EX102_FS      16000U
#define EX102_N       1024U          /* analysed samples: bin = 15.625 Hz */
#define EX102_WARM    256U           /* filter settling, discarded */
#define EX102_AMP     29491          /* 0.9 FS, q1.15 */
#define EX102_SETTLE_US 5U           /* DAC output settling before the ADC samples */
#define EX102_HARM    5U             /* harmonics 2..5 in the THD */
#define EX102_HEADROOM_DB 6.0206f    /* filter input = capture / 2 */

static i16 s_cap[EX102_WARM + EX102_N];
static i16 s_out[EX102_WARM + EX102_N];

/* One tone at bin k through the channel: DAC sample, settle, ADC sample */
static void ex102_capture(u32 k)
{
    const u32 step = (u32)(((u64)(2U * k) << 31) / EX102_N);   /* 2k/N of pi, q1.31 */
    const u32 settle = (SystemCoreClock / 1000000U) * EX102_SETTLE_US;
    u32 ang = 0U;

    for (u32 i = 0U; i < EX102_WARM + EX102_N; i++) {
        i32 s = 0, c = 0;
        (void)ca_cordic_sincos((i32)ang, &s, &c);
        ang += step;
        const i32 v = (i32)(((i64)s * EX102_AMP) >> 31);         /* q1.15 */
        ca_dac_write(DAC_CHANNEL, (u16)(2048 + (v >> 4)));
        const u32 t0 = DWT->CYCCNT;
        while ((DWT->CYCCNT - t0) < settle) { }
        const i32 raw = (i32)ca_adc_read(ADC_NUM, ADC_CHANNEL);
        s_cap[i] = (i16)((raw - 2048) * 16);
    }
    ca_dac_write(DAC_CHANNEL, 2048U);
}

/* Power of the tone at bin k (A^2/2, A = peak of full scale), Goertzel
 * over x[0..N-1]: |X[k]|^2 = p and A = 2|X[k]|/N */
static float ex102_goertzel(const i16 *x, u32 k)
{
    i32 s = 0, c = 0;
    (void)ca_cordic_sincos((i32)(u32)(((u64)(2U * k) << 31) / EX102_N), &s, &c);
    const float coeff = 2.0f * ((float)c / 2147483648.0f);
    float s1 = 0.0f, s2 = 0.0f;
    for (u32 i = 0U; i < EX102_N; i++) {
        const float s0 = (float)x[i] / 32768.0f + coeff * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const float p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0f * p / ((float)EX102_N * (float)EX102_N);
}

/* 10*log10(p) without libm: log2 from the exponent plus an atanh series
 * on the mantissa (error below 1e-4 dB) */
static float ex102_db10(float p)
{
    if (p <= 1e-20f) return -200.0f;
    union { float f; u32 u; } v = { p };
    const i32 e = (i32)((v.u >> 23) & 0xFFU) - 127;
    v.u = (v.u & 0x007FFFFFU) | 0x3F800000U;            /* mantissa in [1,2) */
    const float t = (v.f - 1.0f) / (v.f + 1.0f), t2 = t * t;
    const float ln_m = 2.0f * t * (1.0f + t2 * (1.0f / 3 + t2 * (1.0f / 5 + t2 * (1.0f / 7 + t2 / 9))));
    return 10.0f * ((float)e * 0.30103f + ln_m * 0.4342945f);
}

/* Prints a dB value with two decimals into buf (no %f in caOS printf) */
static const char *ex102_dbtxt(char *buf, float db)
{
    i32 c = (i32)(db * 100.0f + ((db < 0.0f) ? -0.5f : 0.5f));
    const bool neg = c < 0;
    if (neg) c = -c;
    ca_snprintf(buf, 16, "%s%u.%02u", neg ? "-" : "", (u32)c / 100U, (u32)c % 100U);
    return buf;
}

typedef struct { float level, snr, thd; } ex102_meas_t;

/* Level (dB of full scale), SNR and THD of a capture, tone at bin k */
static ex102_meas_t ex102_measure(const i16 *x, u32 k)
{
    float sum = 0.0f, sum2 = 0.0f;
    for (u32 i = 0U; i < EX102_N; i++) {
        const float v = (float)x[i] / 32768.0f;
        sum += v; sum2 += v * v;
    }
    const float mean = sum / (float)EX102_N;
    const float ac = sum2 / (float)EX102_N - mean * mean;      /* total AC power */
    const float p1 = ex102_goertzel(x, k);
    float ph = 0.0f;
    for (u32 h = 2U; h <= EX102_HARM && h * k < EX102_N / 2U; h++) {
        ph += ex102_goertzel(x, h * k);
    }
    float pn = ac - p1 - ph;
    if (pn < 1e-12f) pn = 1e-12f;
    ex102_meas_t m = { ex102_db10(p1 * 2.0f), ex102_db10(p1 / pn), ex102_db10(ph / p1) };
    return m;
}

static u32 ex102_bin(u32 hz)
{
    u32 k = (hz * EX102_N + EX102_FS / 2U) / EX102_FS;
    if (k < 1U) k = 1U;
    if (k > EX102_N / 2U - 1U) k = EX102_N / 2U - 1U;
    return k;
}

/* Tone through channel and filter: returns false when nothing comes back.
 * The channel is measured at 0.9 FS, the filter gets it 6 dB lower. An IIR
 * driven near full scale has a second stable state: hp100 at 500 Hz, 0.9 FS,
 * clip on, 4 runs out of 20 on the U575 settled clipping every period, +1.36 dB
 * at the tone (bit-exact with the model on the captured input, so the filter,
 * not the FMAC). Clip stops the wrap, only headroom stops this. */
static bool ex102_tone(u32 k, ex102_meas_t *in, ex102_meas_t *out)
{
    ex102_capture(k);
    *in = ex102_measure(&s_cap[EX102_WARM], k);
    if (in->level < -30.0f) return false;
    for (u32 i = 0U; i < EX102_WARM + EX102_N; i++) s_cap[i] = (i16)(s_cap[i] >> 1);
    if (!ca_fmac_start(&s_filters[s_sel].f)) return false;
    const u32 n = ca_fmac_run(s_cap, s_out, EX102_WARM + EX102_N);
    ca_fmac_stop();
    if (n != EX102_WARM + EX102_N) return false;
    *out = ex102_measure(&s_out[EX102_WARM], k);
    out->level += EX102_HEADROOM_DB;            /* as if fed at the channel level */
    return true;
}

static void ex102_nosignal(void)
{
    CLI_PUT_N("FMS no signal on the ADC: is the jumper in place?");
    CLI_PRINTF_N("FMS   %s", WIRE_TXT);
}

static void ex102_cmd_tone(u32 hz)
{
    const u32 k = ex102_bin(hz);
    const u32 mhz = k * (EX102_FS * 1000U / EX102_N);
    ex102_meas_t in, out;
    char b1[16], b2[16], b3[16];
    if (!ex102_tone(k, &in, &out)) { ex102_nosignal(); return; }
    CLI_PRINTF_N("FMS tone %u.%03u Hz (bin %u), filter %s", mhz / 1000U, mhz % 1000U, k,
                 s_filters[s_sel].name);
    CLI_PRINTF_N("FMS   channel  level %s dBFS  SNR %s dB  THD %s dB",
                 ex102_dbtxt(b1, in.level), ex102_dbtxt(b2, in.snr), ex102_dbtxt(b3, in.thd));
    CLI_PRINTF_N("FMS   filtered level %s dBFS  SNR %s dB  THD %s dB",
                 ex102_dbtxt(b1, out.level), ex102_dbtxt(b2, out.snr), ex102_dbtxt(b3, out.thd));
    CLI_PRINTF_N("FMS   gain %s dB", ex102_dbtxt(b1, out.level - in.level));
    CLI_PUT_N("FMS   (filter fed 6 dB below the channel for headroom, levels referred back)");
}

static void ex102_cmd_sweep(u32 f0, u32 f1, u32 step)
{
    char b1[16], b2[16], b3[16];
    u32 n = 0U;
    CLI_PRINTF_N("FMS sweep %s  %u..%u Hz step %u   (Hz, in dBFS, out dBFS re channel, gain dB)",
                 s_filters[s_sel].name, f0, f1, step);
    for (u32 hz = f0; hz <= f1; hz += step) {
        const u32 k = ex102_bin(hz);
        const u32 mhz = k * (EX102_FS * 1000U / EX102_N);
        ex102_meas_t in, out;
        if (!ex102_tone(k, &in, &out)) { ex102_nosignal(); return; }
        CLI_PRINTF_N("FMS %5u.%03u %8s %8s %8s", mhz / 1000U, mhz % 1000U,
                     ex102_dbtxt(b1, in.level), ex102_dbtxt(b2, out.level),
                     ex102_dbtxt(b3, out.level - in.level));
        n++;
    }
    CLI_PRINTF_N("FMS END %u", n);
}

/* ============================================================================
 * CLI
 * ========================================================================== */

#if ENABLE_CLI
static const char *ex102_num(const char *p, u32 *v)
{
    p = ca_skip_sp(p);
    if (*p < '0' || *p > '9') return NULL;
    *v = (u32)ca_strtol(p, 10);
    while (*p >= '0' && *p <= '9') p++;
    return p;
}

static void ex102_usage(void)
{
    CLI_PUT_N("fmac      : test | dump | filter [lp1k|bp1k|hp100|lp1k_iir]");
    CLI_PUT_N("            tone <hz> | sweep [f0 f1 step]");
    CLI_PUT_N("wire      : DAC -> ADC jumper continuity as plain GPIO, then reboot");
}

/* DIGITAL continuity of the jumper, as ex27: a missing wire and a broken
 * converter give the same "no signal", this tells them apart. DAC and ADC are
 * stopped first (the DAC buffer and the ADC pad switch would fight the GPIO),
 * the pins become plain GPIO, and the board reboots to get them back. */
static void ex102_wire(void)
{
    ca_dac_stop(DAC_CHANNEL);
    ca_adc_stop(ADC_NUM);
    ca_gpio_init_port(WIRE_OUT_PORT);
    ca_gpio_init_port(WIRE_IN_PORT);
    ca_gpio_configure(WIRE_OUT_PORT, WIRE_OUT_PIN, CA_GPIO_MODE_OUTPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_configure(WIRE_IN_PORT, WIRE_IN_PIN, CA_GPIO_MODE_INPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

    ca_gpio_write(WIRE_OUT_PORT, WIRE_OUT_PIN, 0);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 lo = ca_gpio_read(WIRE_IN_PORT, WIRE_IN_PIN);
    ca_gpio_write(WIRE_OUT_PORT, WIRE_OUT_PIN, 1);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 hi = ca_gpio_read(WIRE_IN_PORT, WIRE_IN_PIN);
    const bool ok = (lo == 0U && hi == 1U);

    CLI_PUT_N("  " WIRE_TXT);
    CLI_PRINTF_N("  drive0->read%u  drive1->read%u  %s", lo, hi,
                 ok ? "OK (jumper present)" : "FAIL (jumper missing or wrong)");
    /* The line the bench reads, identical in every example with `wire` */
    CLI_PRINTF_N("  WIRE RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
    CLI_PUT_N("  rebooting in 2.5 s to restore the analog measurement...");
    ca_os_sleep(2500);
    ca_os_reset();
}

static bool ex102_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { ex102_usage(); return false; }
    if (ca_strncmp(p, "wire", 4) == 0) { ex102_wire(); return true; }
    if (ca_strncmp(p, "fmac", 4) != 0) return false;
    p = ca_skip_sp(p + 4);

    if (ca_strncmp(p, "test", 4) == 0) { ex102_test(); return true; }
    if (ca_strncmp(p, "dump", 4) == 0) { ex102_dump(); return true; }
    if (ca_strncmp(p, "filter", 6) == 0) {
        p = ca_skip_sp(p + 6);
        for (u32 i = 0U; *p != '\0' && i < EX102_NFIL; i++) {
            const u32 len = ca_strlen(s_filters[i].name);
            if (ca_strncmp(p, s_filters[i].name, len) == 0 && (p[len] == '\0' || p[len] == ' '))
                s_sel = i;
        }
        const ca_fmac_filter_t *f = &s_filters[s_sel].f;
        CLI_PRINTF_N("FMS filter %s: %s, %u+%u taps, gain 2^%u", s_filters[s_sel].name,
                     (f->na == 0U) ? "FIR" : "IIR", f->nb, f->na, f->gain);
        return true;
    }
    if (ca_strncmp(p, "tone", 4) == 0) {
        u32 hz = 0U;
        if (ex102_num(p + 4, &hz) == NULL || hz == 0U) { ex102_usage(); return true; }
        ex102_cmd_tone(hz);
        return true;
    }
    if (ca_strncmp(p, "sweep", 5) == 0) {
        u32 f0 = 100U, f1 = 4000U, st = 100U;
        const char *q = ex102_num(p + 5, &f0);
        if (q != NULL) {
            q = ex102_num(q, &f1);
            if (q == NULL || ex102_num(q, &st) == NULL) { ex102_usage(); return true; }
        }
        if (st == 0U || f1 < f0 || f1 > EX102_FS / 2U) { ex102_usage(); return true; }
        ex102_cmd_sweep(f0, f1, st);
        return true;
    }
    ex102_usage();
    return true;
}
static ca_cli_node_t s_cli = { ex102_cli, NULL };
#endif

PRIVATE_STRING(why, "ex102: this part has no FMAC (H563 and U575 have it)");

void ca_os_configure(void)
{
    if (!ca_fmac_init() || !ca_cordic_init()) {
        /* The line the bench matches to know this example cannot run on
           this part. The reason follows it. */
        CA_EXAMPLE_UNSUPPORTED();
        PUT_N(&priv_why);
        return;
    }

    ca_dac_config_t dac_cfg = { DAC_CHANNEL };
    ca_dac_init(&dac_cfg);
    ca_dac_write(DAC_CHANNEL, 2048U);
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
        ca_cli_register(&s_cli);
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else  /* EN_FMAC && EN_DAC && EN_ADC */

PRIVATE_STRING(why, "ex102: built without EN_FMAC/EN_DAC/EN_ADC (FMAC: paid edition, H563/U575)");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_FMAC && EN_DAC && EN_ADC */
