// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * vd6283_ctrl.c — VD6283TX computation: no I2C, no hardware.
 *
 * The OTP bit unpacking, the calibration maths, the autogain solver and the
 * saturation and normalisation helpers. See vd6283_ctrl.h for the split.
 */

#include "vd6283_ctrl.h"

#if EN_I2C

/* The gains, 8.8 fixed point, straight from GainRange[] in the ST driver.
 *
 * ⚠️ Three sources give three different sets and they are all "right" about
 * different things: §4.3.13 lists the nominal codes (66.6x, 16x, 7.1x), Table
 * 16 gives the TYPICAL measured scaling (66.78, 16.76, 7.17), and the driver
 * uses the values below (66.67, 16.67, 7.14). The driver's are the ones to
 * keep: they are what set_channel_gain() rounds against and what the OTP
 * cal10/cal50 split (GAIN_CAL_SPLIT) is expressed in, so a different table
 * would pick a different coefficient for the same register code. */
const u16 GAIN_VALUE[GAIN_TABLE_SIZE] = {
    0x42ABu, 0x3200u, 0x2154u, 0x1900u, 0x10ABu, 0x0A00u, 0x0723u, 0x0500u,
    0x0354u, 0x0280u, 0x01ABu, 0x0140u, 0x0100u, 0x00D4u, 0x00B5u
};

const u32 GAIN_MILLI[GAIN_TABLE_SIZE] = {
    66670u, 50000u, 33330u, 25000u, 16670u, 10000u, 7140u, 5000u,
    3330u,  2500u,  1670u,  1250u,  1000u,  830u,   710u
};

/* Channel order is fixed by the part: DS13735 Table 2 "channel affectation".
 * CH1..CH6 = red, visible, blue, green, IR, clear - so the register at
 * 0x06+4*ch and this table line up by construction.
 *
 * There is no seventh entry: the DARK photodiodes have no data register of
 * their own. Table 3 calls them "1 internal dark channel" and Figure 1 draws
 * them with an amplifier but no ADC. They are read through channel 2 instead:
 * SEL_PD_x(1) selects either the three visible photodiodes (0x07) or the two
 * covered ones (0x18), so dark and visible share one ADC and cannot be sampled
 * at the same time. That is why VIS is reconstructed as k*(R+G+B).
 *
 * The dark count is NOT subtracted from the colour channels, here or anywhere
 * else: the ST driver only ever multiplies (apply_calfactor is k*raw, no
 * additive term), and the datasheet never claims the part compensates its own
 * dark current - Figure 1 shows a connection, which is not an operation.
 * Leaving it in is deliberate and correct for this application: dark(T) is the
 * quantity being measured, not an error to remove, and a 1:1 subtraction would
 * be wrong anyway because the two covered photodiodes do not match the 4-5
 * photodiodes of each colour channel in area, filter or doping.
 *
 * clear and visible are not redundant: clear has NO filter (visible + IR),
 * visible is IR-filtered. Their difference is the IR content of the light. */
const char *const CH_NAME[ALS_CHANNELS] = {
    "red", "vis", "blue", "green", "IR", "clear"
};

/* Saturation level for the exposure currently in force.
 *
 * Table 16 splits on the STEP COUNT - 0x001..0x047 against 0x048..0x3FF - not
 * on a duration, so that is what is compared here. Deriving milliseconds first
 * and testing them against 113 put step 0x47 on the wrong side of the split. */
u32 als_sat_level(u16 exp_steps)
{
    if (exp_steps > ALS_SAT_STEP_THR) return ALS_SAT_LONG_EXP;
    /* Short exposure: the ceiling grows with the number of steps integrated. */
    u32 v = (u32)exp_steps * ALS_SAT_STEP_COUNTS;
    return (v > ALS_SAT_LONG_EXP) ? ALS_SAT_LONG_EXP : v;
}

/* The sensor table. Designated initialisers: the rest (counters, flags) is
 * zeroed by the C runtime and filled in by ca_os_configure. */
sensor_ctx_t s_sensor[SENSOR_COUNT] = {
    /* meas_div and k_dark are seeded so that "lux meas" with no arguments has
     * something to report before the mode has ever been armed. meas_on stays
     * false: the mode is entered only by the command. */
    { .bus = I2C_BUS_A, .alt = I2C_A_ALT, .label = "A", .pins = BUS_A_TXT,
      .exp_steps = EXTIME_STEPS, .gain_idx = GAIN_IDX_DEFAULT, .otp_on = true,
      .ch2_clamp = true,
      .ag_k = AG_K_TYPICAL, .meas_div = 10u, .k_dark = K_DARK_SCALE,
      .meas_discard = MEAS_SWITCH_DISCARD,
      /* ⚠️ meas_ch must be spelled out: MEAS_CH_VIS is 1, and the zero the C
       * runtime would leave is CH_RED - a mode nobody asked for, reported as
       * if it were the default one. im_steps = 0 IS the old behaviour (no idle
       * gap) and is written anyway, next to the exposure it pairs with. */
      .meas_ch = MEAS_CH_VIS, .im_steps = 0u,
      .otp = { .hf_trim = VD6283_HF_TRIM, .lf_trim = VD6283_LF_TRIM } },
    { .bus = I2C_BUS_B, .alt = I2C_B_ALT, .label = "B", .pins = BUS_B_TXT,
      .exp_steps = EXTIME_STEPS, .gain_idx = GAIN_IDX_DEFAULT, .otp_on = true,
      .ch2_clamp = true,
      .ag_k = AG_K_TYPICAL, .meas_div = 10u, .k_dark = K_DARK_SCALE,
      .meas_discard = MEAS_SWITCH_DISCARD,
      /* ⚠️ meas_ch must be spelled out: MEAS_CH_VIS is 1, and the zero the C
       * runtime would leave is CH_RED - a mode nobody asked for, reported as
       * if it were the default one. im_steps = 0 IS the old behaviour (no idle
       * gap) and is written anyway, next to the exposure it pairs with. */
      .meas_ch = MEAS_CH_VIS, .im_steps = 0u,
      .otp = { .hf_trim = VD6283_HF_TRIM, .lf_trim = VD6283_LF_TRIM } },
};

/* Exposure in ms actually in force on one sensor. */
u32 exposure_ms(const sensor_ctx_t *s) { return STEPS_TO_MS(s->exp_steps); }

i32 meas_correct(u32 sig, u32 dark, u32 k_dark, bool *miscal)
{
    /* 64-bit product before the division: dark can reach the 24-bit ceiling and
     * K_DARK_MAX is 100000, so the intermediate runs to ~1.7e12 - well past u32
     * and well inside u64. */
    const u64 sub = ((u64)dark * (u64)k_dark) / K_DARK_SCALE;

    /* Signed in 64 bits, then narrowed. Both terms are bounded well inside
     * i32 - the signal by the 24-bit ceiling, the subtracted term by
     * K_DARK_MAX x that, or ~1.7e9 - but the SUBTRACTION is the one place a
     * wrap would produce a plausible wrong number instead of an obvious one,
     * so it is done where it cannot. */
    const i64 res = (i64)sig - (i64)sub;

    /* ⚠️ `sub > 0` is part of the condition, not a shortcut. With Kdark=0 the
     * subtraction is a no-op, so it CANNOT push anything below zero - and a
     * marker naming a subtraction that never happened sends the reader after
     * the wrong fault. Seen on HW during a Kdark=0 run: rows flagged for a
     * zero that had come from an unrefreshed frame.
     *
     * The threshold is a fraction of what was subtracted, not an absolute
     * count: the same Kdark error produces a residue proportional to the dark,
     * so a fixed count would fire at high gain and stay silent at low. */
    if (miscal)
        *miscal = (sub > 0u) &&
                  (res < 0) &&
                  ((u64)(-res) > (sub * MEAS_MISCAL_PCT) / 100u);

    if (res >  (i64)0x7FFFFFFF) return  (i32)0x7FFFFFFF;
    if (res < -(i64)0x7FFFFFFF) return -(i32)0x7FFFFFFF;
    return (i32)res;
}

u8 otp_bit_reverse(u8 d)
{
    d = (u8)(((d & 0xAAu) >> 1) | ((d & 0x55u) << 1));
    d = (u8)(((d & 0xCCu) >> 2) | ((d & 0x33u) << 2));
    d = (u8)(((d & 0xF0u) >> 4) | ((d & 0x0Fu) << 4));
    return d;
}

/*
 * Extract one field. `swap` reverses the bit order WITHIN the field, which the
 * OTP layout needs for every value this port reads.
 */
u32 otp_field(const u64 bits[2], u32 bit_start, u32 bit_nb, bool swap)
{
    const u32 bit_end = bit_start + bit_nb - 1u;
    u32 bank0 = 0, bank1 = 0, shift = 0, result;

    if (bit_nb > 24u || bit_end >= 120u) return 0;

    if (bit_start < 64u) {
        u32 end = (bit_end < 63u) ? bit_end : 63u;
        u32 n   = end - bit_start + 1u;
        bank0 = (u32)((bits[0] >> bit_start) & (((u64)1 << n) - 1u));
        shift = n;
    }
    if (bit_end >= 64u) {
        u32 lo = (bit_start > 64u) ? (bit_start - 64u) : 0u;
        u32 n  = (bit_end - 64u) - lo + 1u;
        bank1 = (u32)((bits[1] >> lo) & (((u64)1 << n) - 1u));
    }

    result = bank0 | (bank1 << shift);

    if (swap) {
        u32 rev = 0;
        for (u32 i = 0; i < bit_nb; i++)
            if ((result >> i) & 1u) rev |= 1u << (bit_nb - 1u - i);
        result = rev;
    }
    return result;
}

/* Nominal calibration: what the part looks like with no OTP. */
void otp_defaults(vd_otp_t *o)
{
    o->read    = false;
    o->hf_trim = VD6283_HF_TRIM;
    o->lf_trim = VD6283_LF_TRIM;
    o->version = 0;
    for (u8 c = 0; c < ALS_CHANNELS; c++) o->gains[c] = VD6283_DEFAULT_GAIN;
    o->uid[0]  = '?';
    o->uid[1]  = '\0';
}

/*
 * The part's unique id, reproducing ST's otp_generate_uid() exactly.
 *
 * The first nibble is the OTP version (3 bits at 113) shifted up by one, with
 * bit 60 in the low position; the remaining thirteen come from bits 61..111,
 * four at a time. Each nibble is XORed with the running total of the ones
 * before it, then rendered as a hex character - so a single wrong bit anywhere
 * changes every character from that point on, which is what makes two parts
 * impossible to confuse by eye.
 */
void otp_uid(vd_otp_t *o, const u64 bits[2])
{
    /* ST's encode_nible() returns `nible ^ prev_xor` and stores the same value
     * back into the register, so the running total IS the character just
     * emitted. Written out, that is the two lines in the loop below. */
    u8 xor_reg = 0;

    for (u8 n = 0; n < 14u && n < VD_UID_LEN - 1u; n++) {
        u8 nib;

        if (n == 0u) {
            /* First nibble: OTP version in the top three bits, bit 60 below. */
            nib  = (u8)(otp_field(bits, 113u, 3u, true) << 1);
            nib |= (u8)otp_field(bits, 60u, 1u, true);
        } else {
            nib = (u8)otp_field(bits, 61u + (u32)(n - 1u) * 4u, 4u, true);
        }

        xor_reg = (u8)((nib ^ xor_reg) & 0x0Fu);
        o->uid[n] = (char)(xor_reg < 10u ? ('0' + xor_reg)
                                         : ('a' + xor_reg - 10u));
    }
    o->uid[14] = '\0';
}

u8 otp_calfactor(u8 otp_gain, u16 channel_gain)
{
    u8 g = (channel_gain < GAIN_CAL_SPLIT) ? (u8)((otp_gain & 0xF0u) >> 4)
                                           : (u8)(otp_gain & 0x0Fu);
    return (g & 0x08u) ? (u8)(0x80u | ((g & 0x07u) << 2))
                       : (u8)((0x20u - g) << 2);
}

/* Apply it to one count. The >>7 matches the 128 = x1.000 scale.
 *
 * ⚠️ ST's apply_calfactor() also floors anything below 0x100 to zero, on the
 * grounds that the correction there is smaller than the noise. That is NOT
 * reproduced here, deliberately: this application is a measurement bench, and
 * the floor deletes real readings.
 *
 * Seen twice on HW:
 *  - dark mode: sensor A (gain 7.13x) reported dark=318 while sensor B
 *    (2.50x) reported 0 on the same bench - B's ~118 counts were simply
 *    erased, not absent;
 *  - dim light: sensor A reported RED=0 and BLUE=0 while GREEN=256 and
 *    IR=795, because those two channels sat just under the threshold.
 *
 * Below 250 counts the reading is noisy, and it is the operator's business to
 * know that - a zero that means "small" is indistinguishable from a zero that
 * means "nothing", and the second is a fault. */
u32 otp_apply(u32 raw, u8 factor)
{
    /* ⚠️ NOT clamped, deliberately. The coefficient can carry a value above the
     * saturation level - that is normal and informative, and it used to be
     * visible (a clipped clear reported 79870 against a ceiling of 65535).
     *
     * The clamp was harmless while counts lived on the 16-bit scale, because
     * ALS_FULL_SCALE_24 sat 256x higher and never bit. On the 24-bit scale it
     * coincides EXACTLY with the long-exposure ceiling, so a corrected value
     * that overflowed became indistinguishable from a saturated raw one -
     * seen on HW as VIS=16777215 with no channel actually pinned, and as an
     * NVIS identical on two different sensors. `pinned`, which tests the RAW
     * count, stays the only authority on saturation.
     *
     * No overflow risk: factor <= 156 and raw <= 0xFFFFFF give at most ~20.4e6
     * after the shift, well inside u32. */
    return (u32)(((u64)factor * (u64)raw) >> 7);
}

/* Percent of full scale, on the RAW count: the decision is about the ADC's
 * headroom, which the per-part correction does not change. */
u32 ag_peak_pct(const lux_sample_t *smp)
{
    u32 sat = smp->sat ? smp->sat : ALS_SAT_LONG_EXP;
    u32 pct = (u32)(((u64)smp->raw[5] * 100u) / sat);
    return (pct > 100u) ? 100u : pct;
}

/* Mean of red, green and blue - the channels still on scale when clear is not. */
u32 ag_rgb_mean(const lux_sample_t *smp)
{
    return (smp->raw[0] + smp->raw[3] + smp->raw[2]) / 3u;
}

bool ag_solve(u32 est, u8 gi, u16 st, u8 *out_gi, u16 *out_st)
{
    const u32 target = (u32)(((u64)ALS_SAT_LONG_EXP * AG_TARGET_PCT) / 100u);
    if (est < 1u) est = 1u;

    /* Wanted total scaling = current x (target/est), in gain*steps units.
     *
     * ⚠️ One 64-bit product, then the division. The scaled-ratio form this
     * replaced - (target*1024/est) evaluated first, then multiplied - overflows
     * once target is on the 24-bit scale: with est down at 1 (clear reading
     * zero in the dark) the intermediate reaches ~2e19 and wraps. Multiplying
     * first keeps the worst case at ~2e16, well inside u64, and loses no
     * precision to the truncation the 1024 scaling existed to hide. */
    u64 cur = (u64)GAIN_VALUE[gi] * ((u64)st + 1u);
    u64 tot = (cur * (u64)target) / est;
    if (tot < 1u) tot = 1u;

    /* Beyond the ends: clamp and report that no solution exists. */
    if (tot >= (u64)GAIN_VALUE[0] * ((u64)EXTIME_STEPS_MAX + 1u)) {
        *out_gi = 0; *out_st = (u16)EXTIME_STEPS_MAX; return false;
    }
    if (tot <= (u64)GAIN_VALUE[GAIN_TABLE_SIZE - 1u] * ((u64)EXTIME_STEPS_MIN + 1u)) {
        *out_gi = (u8)(GAIN_TABLE_SIZE - 1u); *out_st = (u16)EXTIME_STEPS_MIN; return false;
    }

    /* Pick the (gain, exposure) pair closest to the target, preferring the
     * DEFAULT exposure: gain costs nothing, while exposure changes how long a
     * sample takes - so it moves only when the gain cannot do the job alone. */
    u8  bg = gi; u32 bs = st; u64 best = 0; bool found = false;
    for (u8 g = 0; g < GAIN_TABLE_SIZE; g++) {
        u64 s1 = tot / GAIN_VALUE[g];
        if (s1 < 1u) s1 = 1u;
        if (s1 - 1u > EXTIME_STEPS_MAX) continue;
        u32 steps = (u32)(s1 - 1u);
        if (steps < EXTIME_STEPS_MIN) steps = EXTIME_STEPS_MIN;

        u64 got = (u64)GAIN_VALUE[g] * ((u64)steps + 1u);
        u64 err = (got > tot) ? (got - tot) : (tot - got);
        u64 pen = (steps > EXTIME_STEPS) ? (steps - EXTIME_STEPS)
                                         : (EXTIME_STEPS - steps);
        u64 score = err + pen * 64u;    /* 64 = how dearly exposure is bought */

        if (!found || score < best) { best = score; bg = g; bs = steps; found = true; }
    }
    *out_gi = bg; *out_st = (u16)bs;
    return found;
}

bool ag_update(sensor_ctx_t *s, const lux_sample_t *smp)
{
    u32  pct   = ag_peak_pct(smp);
    u32  mean  = ag_rgb_mean(smp);
    u32  sat   = smp->sat ? smp->sat : ALS_SAT_LONG_EXP;
    bool pinned = (smp->raw[5] >= sat);
    u32  est;

    /* Keep the ratio current from every sample where both scales are valid. */
    if (!pinned && mean > 0u)
        s->ag_k = (u32)(((u64)smp->raw[5] * AG_K_SCALE) / mean);

    if (pinned) {
        /* Clear says only "at least full scale". Estimate what it would have
         * read from the RGB mean, so the correction knows HOW FAR over it is
         * instead of creeping down one index at a time. */
        est = (mean > 0u) ? (u32)(((u64)mean * s->ag_k) / AG_K_SCALE) : sat;
        if (est < sat) est = sat;       /* it is pinned: never estimate below */
    } else {
        est = smp->raw[5];
        if (pct >= AG_LOW_PCT && pct <= AG_HIGH_PCT) return false;  /* in band */
    }

    u8  ngi; u16 nst;
    (void)ag_solve(est, s->gain_idx, s->exp_steps, &ngi, &nst);
    if (ngi == s->gain_idx && nst == s->exp_steps) return false;    /* nothing to do */

    s->gain_idx  = ngi;
    s->exp_steps = nst;
    return true;
}

/* Gain as "N.MM" without floating point: ca_vsnprintf has no %f. */
void gain_text(u8 idx, u32 *whole, u32 *frac)
{
    u32 m = GAIN_MILLI[idx < GAIN_TABLE_SIZE ? idx : GAIN_IDX_1X];
    *whole = m / 1000u;
    *frac  = (m % 1000u) / 10u;     /* two decimals */
}

/* Scale a count to the reference gain and exposure (see NORM_GAIN). Returns 0
 * when the sample carries no valid settings, which the caller prints as "--". */
u32 norm_count(u32 v, u16 gain, u16 exp_steps)
{
    if (v == 0u || gain == 0u) return 0u;
    /* v * (NORM_GAIN/gain) * NORM_STEPS/steps: one 64-bit expression so the two
     * divisions cannot each throw away a few percent.
     *
     * ⚠️ The +1 that used to sit on both sides is gone, and it did NOT cancel:
     * (s+1)/(r+1) against s/r is a factor of two at s=1. See EXTIME_PLUS_ONE -
     * Table 16 and ST's set_exposure() both give exposure = EXTIME x 1.6 ms
     * with no offset, and the claim that the ratio was the same either way was
     * simply wrong. */
    const u64 num = (u64)v * NORM_GAIN * (u64)EXTIME_UNITS(NORM_STEPS);
    const u64 den = (u64)gain * (u64)EXTIME_UNITS(exp_steps);
    return (u32)(num / den);
}

/* ============================================================================
 * PHOTOMETRY - lux and CCT (see LUX_MATRIX in the header)
 * ========================================================================== */

/* ST's coefficients from als_cct_lux.c, x1e6. Rows X, Y, Z; columns R, G, B.
 *
 *      { 0.205570,  0.416700, -0.143816 }
 *      {-0.028752,  0.506372, -0.120614 }
 *      {-0.552625,  0.335866,  0.494781 }
 *
 * ⚠️ Nine numbers copied by hand is exactly where a transcription slip hides
 * (channel 4's SEL_PD was one, 0x0d for 0x1f), so they are laid out in the
 * same order and the same grouping as the source they came from. */
const i32 LUX_MATRIX[3][3] = {
    {  205570,  416700, -143816 },
    {  -28752,  506372, -120614 },
    { -552625,  335866,  494781 }
};

bool lux_photometric(const u32 counts[ALS_CHANNELS], u16 gain, u16 exp_steps,
                     lux_photo_t *out)
{
    if (!out) return false;
    out->lux_milli = 0; out->cct_k = 0;
    out->x_ten_k = 0;   out->y_ten_k = 0;
    out->lux_ok = false; out->cct_ok = false;

    if (gain == 0u || exp_steps == 0u) return false;

    /* Step 1 - out of gain and exposure, onto the reference exposure.
     *
     * ST writes it as (REF/expo) * (count/256) / (gain/256); the two 256s are
     * the 8.8 gain and the fixed-point count, and they cancel. What is left is
     *
     *     norm = REF_US * count / (expo_us * gain_8_8)
     *
     * evaluated as ONE ratio, x LUX_NORM_SCALE, so neither division throws
     * away the small channels - which in darkness is all of them. See
     * LUX_NORM_SCALE for what the scale costs at both ends; the clamp below is
     * the overflow bound it documents, enforced rather than assumed. */
    const u64 expo_us = (u64)EXTIME_UNITS(exp_steps) * EXTIME_US_PER_STEP;
    const u64 den     = expo_us * (u64)gain;
    if (den == 0u) return false;

    i64 norm[3];    /* R, G, B - in the matrix's column order */
    {
        const u8 col[3] = { (u8)CH_RED, (u8)CH_GREEN, (u8)CH_BLUE };
        for (u8 i = 0; i < 3u; i++) {
            const u64 num = (u64)counts[col[i]] * LUX_REF_EXPO_US
                          * (u64)LUX_NORM_SCALE;
            i64 n = (i64)(num / den);
            if (n > LUX_NORM_MAX) n = LUX_NORM_MAX;
            norm[i] = n;
        }
    }

    /* Step 2 - the matrix. Signed throughout: the coefficients are, and so
     * therefore is the result. Bounded by the clamp above: the widest row of
     * |LUX_MATRIX| sums to 1383272 over its three columns, so acc <= 1383272 x
     * LUX_NORM_MAX = 6.9e18, inside i64's 9.22e18. That inequality is the only
     * reason LUX_NORM_MAX is 5e12 and not larger. */
    i64 xyz[3];
    for (u8 r = 0; r < 3u; r++) {
        i64 acc = 0;
        for (u8 c = 0; c < 3u; c++)
            acc += (i64)LUX_MATRIX[r][c] * norm[c];
        xyz[r] = acc / LUX_MATRIX_SCALE;      /* still x LUX_NORM_SCALE */
    }

    const i64 sum = xyz[0] + xyz[1] + xyz[2];

    /* ⚠️ ST checks only "sum == 0" before dividing. That is not enough here:
     * the matrix has negative rows, so in darkness - where the channels are a
     * few counts of noise and the negative terms can win - X, Y or the sum
     * come out NEGATIVE. Dividing then yields a chromaticity outside the
     * diagram and a CCT that looks like a reading. A threshold alarm lives in
     * exactly that regime, so this is the case to get right, not an edge. */
    if (sum <= 0 || xyz[1] < 0) return false;

    /* Step 3a - lux IS Y. Not a simplification made here: STALS_LIB_compute_lux
     * takes X, Y, Z and returns Y, casting X and Z to void. */
    {
        /* Clamped rather than truncated: xyz[1] is bounded by the matrix, not
         * by u32, and a wrap here would turn a saturated reading into a small
         * plausible one - the failure mode this whole file is written against. */
        const u64 lm = ((u64)xyz[1] * 1000u) / LUX_NORM_SCALE;
        out->lux_milli = (lm > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (u32)lm;
        out->lux_ok    = true;
    }

    /* Step 3b - chromaticity and McCamy's cubic.
     *
     *     n   = (x - 0.3320) / (0.1858 - y)
     *     CCT = 449 n^3 + 3525 n^2 + 6823.3 n + 5520.33
     *
     * x and y are held x10000 and n x1000. The denominator vanishes on the
     * line y = 0.1858, where the formula has no answer - a real singularity,
     * not a rounding problem, so it is reported rather than clamped. */
    const i64 x_tk = ((i64)xyz[0] * 10000) / sum;
    const i64 y_tk = ((i64)xyz[1] * 10000) / sum;
    out->x_ten_k = (u32)(x_tk < 0 ? 0 : x_tk);
    out->y_ten_k = (u32)(y_tk < 0 ? 0 : y_tk);

    const i64 nden = 1858 - y_tk;
    if (nden == 0) return true;              /* lux stands, CCT does not */

    const i64 n_m = ((x_tk - 3320) * 1000) / nden;    /* n x1000 */

    /* ⚠️ Bound n BEFORE cubing it, not after. Near the singularity the ratio
     * grows without limit, and n_m^3 overflows i64 at |n| ~ 2e6 - which would
     * wrap to a small positive number and land inside the plausibility window
     * below as a perfectly ordinary-looking colour temperature. |n| > 100 is
     * already far outside anything McCamy's fit describes. */
    if (n_m > 100000 || n_m < -100000) return true;   /* lux stands, CCT does not */

    /* 449 n^3 + 3525 n^2 + 6823.3 n + 5520.33, each term brought back to K.
     * n_m stays modest in the region the cubic is valid in; the guard below is
     * what keeps a wild n from being reported as a colour temperature. */
    const i64 n2 = n_m * n_m;                 /* x1e6 */
    const i64 n3 = n2 * n_m / 1000;           /* x1e6 */
    const i64 cct = (449 * n3) / 1000000
                  + (3525 * n2) / 1000000
                  + (68233 * n_m) / 10000
                  + 5520;

    /* McCamy's approximation is stated for roughly 2000..25000 K and drifts
     * badly outside it. Reporting a number there would dress up an
     * extrapolation as a measurement. */
    if (cct >= 1500 && cct <= 25000) {
        out->cct_k  = (u32)cct;
        out->cct_ok = true;
    }
    return true;
}

#endif /* EN_I2C */
