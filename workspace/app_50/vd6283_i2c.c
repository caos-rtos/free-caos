// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * vd6283_i2c.c — every access to the VD6283TX over I2C.
 *
 * Register reads and writes, the bring-up sequence, frame handshaking, the OTP
 * load and the SEL_PD sweep. The arithmetic these feed lives in
 * vd6283_ctrl.c; see vd6283_ctrl.h for the split.
 *
 * ⚠️ Everything here writes to the bus, so it must be called from the task
 * that owns that sensor - never from the CLI thread while the sensor runs.
 */

#include "vd6283_i2c.h"

#if EN_I2C

bool vd_write_reg(u8 bus, u8 reg, u8 val)
{
    u8 buf[2] = { reg, val };
    return ca_i2c_write(bus, VD6283_ADDR, buf, 2u);
}

bool vd_read_regs(u8 bus, u8 reg, u8 *dst, u16 len)
{
    /* Write the index, then a repeated start and the read. The part
     * auto-increments the index across a sequential read (§3.4.1). */
    return ca_i2c_write_read(bus, VD6283_ADDR, &reg, 1u, dst, len);
}

/* Load both banks into a 2 x 64-bit window. Returns false if either read or
 * the ready handshake fails - the caller then keeps the nominal defaults. */
bool otp_load(sensor_ctx_t *s, u64 bits[2])
{
    u8  reg[8];
    u32 spin;

    /* Reset the OTP reader, then wait for BOTH banks to report ready. The
     * bound is ST's: a part that never asserts them must not hang the task. */
    if (!vd_write_reg(s->bus, VD6283_OTP_CTRL1, 0x00u)) return false;
    for (spin = 0; spin < VD6283_OTP_POLL_MAX; spin++) {
        u8 status = 0;
        if (!vd_read_regs(s->bus, VD6283_OTP_STATUS, &status, 1u)) return false;
        if ((status & VD6283_OTP_DATA_READY) == VD6283_OTP_DATA_READY) break;
    }
    if (spin >= VD6283_OTP_POLL_MAX) return false;

    bits[0] = 0; bits[1] = 0;

    if (!vd_read_regs(s->bus, VD6283_OTP_BANK_0, reg, 8u)) return false;
    for (u8 i = 0; i < 7u; i++)
        bits[0] |= (u64)otp_bit_reverse(reg[i]) << (i * 8u);
    /* Byte 7 contributes only its top nibble: 56 + 4 = 60 bits so far. */
    bits[0] |= (u64)(otp_bit_reverse(reg[7]) >> 4) << 56;

    if (!vd_read_regs(s->bus, VD6283_OTP_BANK_1, reg, 8u)) return false;
    /* The low nibble of bank 1 byte 0 completes bank 0's 64 bits; its high
     * nibble opens bank 1. Everything after it is shifted by that nibble. */
    reg[0] = otp_bit_reverse(reg[0]);
    bits[0] |= (u64)(reg[0] & 0x0Fu) << 60;
    bits[1]  = (u64)(reg[0] >> 4);
    for (u8 i = 1u; i < 7u; i++)
        bits[1] |= (u64)otp_bit_reverse(reg[i]) << (i * 8u - 4u);
    bits[1] |= (u64)(otp_bit_reverse(reg[7]) >> 4) << 52;

    return true;
}

/*
 * Read and decode the factory calibration. Never fails the bring-up: a part
 * whose OTP cannot be read still measures, just without per-part correction,
 * so on failure the defaults stand and `read` stays false. "lux otp" reports
 * which of the two happened - silently substituting defaults would make an
 * uncalibrated sensor indistinguishable from a calibrated one.
 */
void vd_read_otp(sensor_ctx_t *s)
{
    u64 bits[2];

    otp_defaults(&s->otp);
    if (!otp_load(s, bits)) return;

    /* Version is 3 bits at 113, offset by 0x10 for Rainbow2 (ST's comment). */
    s->otp.version = (u8)(otp_field(bits, 113u, 3u, true) + 0x10u);
    s->otp.hf_trim = (u16)otp_field(bits, OTP_BIT_HF_TRIM, 9u, true);
    s->otp.lf_trim = (u8) otp_field(bits, OTP_BIT_LF_TRIM, 4u, true);

    for (u8 c = 0; c < ALS_CHANNELS; c++) {
        u32 g = otp_field(bits, OTP_BIT_GAIN(c), 8u, true);
        /* 0 is not a valid coefficient: ST substitutes the nominal one. */
        s->otp.gains[c] = (u8)(g ? g : VD6283_DEFAULT_GAIN);
    }

    /* The die's identity, from the same bits. It is what ties a thermal
     * calibration to a PART rather than to a bus position - see vd_otp_t. */
    otp_uid(&s->otp, bits);

    s->otp.read = true;
}

/*
 * Acknowledge the current frame and wait for the NEXT one to complete.
 *
 * ⚠️ Waiting a fixed EXPOSURE_MS instead does not work. That measures time on
 * the MCU side, while the frame boundary is set by the sensor's own 10.24 MHz
 * oscillator: the two clocks drift, so the wait sometimes lands just before
 * the boundary and the value read still carries part of the previous frame.
 * With the photodiodes switched to dark that showed up as occasional spikes
 * of a few hundred counts - about 5% of the visible reading, i.e. the tail of
 * a visible frame leaking into a dark one.
 *
 * INTR_ST (§4.3.3) is the authority: 1 means "no interrupt has been
 * triggered", i.e. the measurement is still running; it goes to 0 when the
 * frame is done. The ST driver reads it the same way (is_data_ready:
 * `!(isr_ctrl_st & 0x02)`).
 *
 * Returns false on timeout - a part that stopped producing frames must not
 * hang the task.
 */
bool vd_next_frame(sensor_ctx_t *s)
{
    /* Acknowledge: clears INTR_ST and lets the part start a new frame. */
    if (!vd_write_reg(s->bus, VD6283_INTERRUPT_CTRL, CLR_INTR)) return false;
    if (!vd_write_reg(s->bus, VD6283_INTERRUPT_CTRL, 0x00u))    return false;

    /* Poll rather than sleep the whole window: the first read cannot succeed
     * before one exposure plus the programmed idle gap has elapsed, so wait
     * most of it, then look. Missing the gap here would not break anything -
     * the loop below still waits on INTR_ST - but it would spend the whole
     * period polling the bus at FRAME_POLL_MS, which is the opposite of what
     * the period was raised for. */
    ca_os_sleep(exposure_ms(s) + IM_STEPS_TO_MS(s->im_steps));

    for (u32 waited = 0; waited < FRAME_TIMEOUT_MS(s); waited += FRAME_POLL_MS) {
        u8 st = 0;
        if (!vd_read_regs(s->bus, VD6283_INTERRUPT_CTRL, &st, 1u)) return false;
        if ((st & INTR_ST) == 0u) return true;      /* frame complete */
        ca_os_sleep(FRAME_POLL_MS);
    }
    return false;
}

/*
 * Bring one sensor up in its lowest-noise configuration.
 * Returns false if the part does not answer or does not identify itself.
 */
bool vd_configure(sensor_ctx_t *s)
{
    u8 id[2] = { 0, 0 };

    /* Identify before configuring: a NACK here means wiring or address, and
     * writing blind would leave the caller guessing which.
     *
     * The two failures below are NOT the same fault and must not be reported
     * as one "not answering": a failed transfer means nothing drove the bus,
     * while a wrong DEVICE_ID means something DID answer at this address. */
    if (!vd_read_regs(s->bus, VD6283_DEVICE_ID, id, 2u)) {
        s->fail = FAIL_NO_ACK;
        return false;
    }
    s->dev_id = id[0];
    s->rev_id = id[1];
    if (id[0] != VD6283_DEV_ID_VAL) {
        s->fail = FAIL_BAD_ID;
        return false;
    }
    s->fail = FAIL_NONE;

    /* Read the factory calibration BEFORE configuring: its hf_trim/lf_trim are
     * what the oscillator writes below should carry. A part whose OTP is
     * unreadable still runs, on the nominal defaults. */
    vd_read_otp(s);

    /* Every configuration write must be checked: a part that identified itself
     * and then NACKs a write leaves the ALS engine half-configured, which is a
     * different fault from "not there" and has to be reported as such. */
#define VD_WR(reg, val)  do { if (!vd_write_reg(s->bus, (reg), (val))) { \
                                  s->fail = FAIL_CONFIG; return false; } } while (0)

    /* Stop the state machine before touching its parameters: the exposure and
     * gain registers are latched per measurement, and changing them mid-frame
     * would yield one sample taken with a mix of old and new settings. */
    VD_WR(VD6283_ALS_CTRL, 0x00u);

    /* --- Analog front-end bring-up, mirroring dev_configuration() of the ST
     * driver. None of this is in the datasheet, and skipping it is why the
     * counts were wrong: the part acknowledges every I2C write and answers
     * with plausible-looking numbers regardless. --- */

    /* Oscillator trim (trim_oscillators). EXTIME counts cycles of the 10.24 MHz
     * oscillator, so this sets what an exposure actually lasts. */
    {   /* Per-part values when the OTP was readable, ST's fallbacks otherwise;
         * otp_defaults() has already put the right pair in s->otp. Honouring
         * "otp off" here too keeps the A/B comparison honest: it must switch
         * off the clock correction as well, not just the count coefficient. */
        u16 hf = s->otp_on ? s->otp.hf_trim : (u16)VD6283_HF_TRIM;
        u8  lf = s->otp_on ? s->otp.lf_trim : (u8)VD6283_LF_TRIM;
        VD_WR(VD6283_OSC10M,        0x01u);
        VD_WR(VD6283_OSC10M_TRIM_M, (u8)((hf >> 8) & 0x01u));
        VD_WR(VD6283_OSC10M_TRIM_L, (u8)(hf & 0xFFu));
        VD_WR(VD6283_OSC50K_TRIM,   (u8)(lf & 0x0Fu));
    }

    /* Clamps and count normalisation (clamp_enable). SPARE_1 = 0x3f carries
     * the ST comment "set count to be independent of pd numbers" - it is what
     * makes a channel's count comparable regardless of HOW MANY photodiodes
     * feed it, which is precisely what switching channel 2 between 3 visible
     * diodes and 2 covered ones depends on. */
    VD_WR(VD6283_AC_CLAMP_EN, 0x01u);
    /* Channel 2's clamp is the one switch that decides whether the dark channel
     * can see dark current at all - see DC_CLAMP_ALL. Written here, from the
     * flag, so the choice belongs to a run and can never change under a
     * measurement in progress. */
    VD_WR(VD6283_DC_CLAMP_EN, s->ch2_clamp ? DC_CLAMP_ALL : DC_CLAMP_CH2_OFF);
    VD_WR(VD6283_SPARE_1,     0x3Fu);

    /* ⚠️ Photodiode selection per channel. This is NOT optional setup: at
     * reset these registers do not hold the working values, and the ST driver
     * programs them in select_pd() during STALS_Init - a step a bare-metal
     * port has to reproduce. Symptom when it is missing: channel 2 reads 0
     * forever, because no photodiode is connected to it.
     *
     * Values straight from select_pd(): {07,07,07,1f,0f,1f}. The count of set
     * bits is the number of diodes feeding that channel, which is why clear
     * (0x1f) is among the most sensitive of the six.
     *
     * ⚠️ Channel 4 (green) was 0x0d here, against ST's 0x1f - green integrated
     * on fewer photodiodes than it should and read low against the others.
     * Transcription slip, not a deliberate choice. */
    static const u8 SEL_PD_INIT[ALS_CHANNELS] = {
        0x07u, SEL_PD1_VISIBLE, 0x07u, 0x1Fu, 0x0Fu, 0x1Fu
    };
    for (u8 ch = 0; ch < ALS_CHANNELS; ch++) {
        /* ⚠️ Channel 2 keeps whatever mode was asked for. This loop used to
         * write SEL_PD_INIT unconditionally, so every "lux start" put channel
         * 2 back on the visible diodes while s->dark_mode still said dark:
         * the banner claimed dark mode and the register said 0x07, and the
         * "dark" reading was the visible channel under another name. */
        u8 v = (ch == 1u && s->dark_req) ? SEL_PD1_DARK : SEL_PD_INIT[ch];
        VD_WR(VD6283_SEL_PD(ch), v);
    }
    s->dark_mode = s->dark_req;     /* register and flag now agree */

    /* Exposure, from the runtime value ("lux setup <n> exp"). Low byte first,
     * then the 2-bit high part: that is the order set_exposure() uses. */
    VD_WR(VD6283_EXPOSURE_M + 1u, (u8)(s->exp_steps & 0xFFu));
    VD_WR(VD6283_EXPOSURE_M,      (u8)((s->exp_steps >> 8) & 0x03u));

    /* Analog gain, same on every channel so the six stay comparable with each
     * other. ⚠️ The register takes the table INDEX + 1, not the gain. */
    for (u8 ch = 0; ch < ALS_CHANNELS; ch++) {
        VD_WR((u8)(VD6283_GAIN_CH1 + ch), (u8)(s->gain_idx + 1u));
    }

    /* Oscillator dithering (set_dithering): spreads the clock spectrum. */
    VD_WR(VD6283_DITHER_STEP_VALUE,    0x01u);
    VD_WR(VD6283_DITHER_STEP_DURATION, 0x01u);
    VD_WR(VD6283_DITHER_STEP_NUMBER,   0x09u);
    VD_WR(VD6283_DITHER_CTRL,          0x01u);

    /* Pedestal value 3, the ST default (set_pedestal_value(dev, 3) in
     * dev_configuration). Applies to the flicker path. */
    VD_WR(VD6283_AC_PEDESTAL, 0x03u);

    /* GPIO2 as input, the driver's resting state. */
    VD_WR(VD6283_SPARE_0, VD6283_SPARE_0_IN_GPIO2);

    /* Idle gap between measurements (§4.3.5). 0 is the historical behaviour -
     * back-to-back frames, EXTIME alone setting the pace - and is still the
     * default; "lux period" raises it to slow the sensor down without touching
     * the exposure, which is the only knob that changes the RATE without also
     * changing what each reading means.
     *
     * Written here, from the context, so it takes effect at a start like every
     * other acquisition parameter and can never change under a frame. */
    VD_WR(VD6283_ALS_PERIOD, s->im_steps);

    /* All six channels. Channels 1..5 share one register, channel 6 has its
     * own — enabling "all" needs both writes (§4.3.19, §4.3.20). */
    VD_WR(VD6283_CH_ENABLE,  ALS_CH_ALL_1_5);
    VD_WR(VD6283_CH6_ENABLE, CH6_EN);

    /* Flicker extractor off: it steals the clear channel from ALS (§3.2) and
     * its output is PDM on a GPIO, which this application does not read. */
    VD_WR(VD6283_AC_MODE_CTRL, 0x00u);

    /* Clear any stale interrupt flag before starting, so the first frame is
     * not immediately gated by an acknowledgment left over from a previous
     * run (the part keeps its state across a warm reset of the MCU). */
    VD_WR(VD6283_INTERRUPT_CTRL, CLR_INTR);
    VD_WR(VD6283_INTERRUPT_CTRL, 0x00u);

    /* Start. Mode 3 is continuous but GATED: each frame has to be
     * acknowledged, which vd_read_channels does after every read. */
    VD_WR(VD6283_ALS_CTRL, ALS_CONT_MODE3 | ALS_EN);
#undef VD_WR

    return true;
}

/*
 * Read all six channels. Each channel is a 24-bit big-endian value at
 * 0x06 + 4*ch, so one sequential read covers the lot and gives a coherent
 * snapshot instead of six separate transactions straddling a frame.
 */
bool vd_read_channels(sensor_ctx_t *s, lux_sample_t *out)
{
    /* From 0x06 to 0x1C: five full groups of four plus the three data bytes of
     * the sixth, so 23 bytes rather than 24 - the padding byte that opens each
     * group is skipped by the stride, and there is none to read after the last
     * channel (0x1D is ALS_EXPOSURE_M). The buffer stays 24 for the stride. */
    u8 raw[ALS_CHANNELS * 4u];

    if (!vd_read_regs(s->bus, VD6283_CH1_DATA_H, raw,
                      (u16)(ALS_CHANNELS * 4u - 1u)))
        return false;

    const u16 gain = GAIN_VALUE[s->gain_idx];
    const u32 sat  = als_sat_level(s->exp_steps);

    out->saturated = 0;
    out->sat       = sat;
    out->gain      = gain;
    out->exp_ms    = (u16)exposure_ms(s);
    out->exp_steps = s->exp_steps;      /* what norm_count scales on */
    out->otp_on    = (s->otp_on && s->otp.read) ? 1u : 0u;
    for (u8 ch = 0; ch < ALS_CHANNELS; ch++) {
        const u8 *p = &raw[ch * 4u];
        /* H, M, L - the full 24 bits (§4.3.6). p[0] is 0x06 + 4*ch because the
         * base skips the group's padding byte; see VD6283_CH_DATA. */
        u32 v = ((u32)p[0] << 16) | ((u32)p[1] << 8) | (u32)p[2];
        out->raw[ch] = v;
        /* §3.3.4: judge saturation on the RAW count, against the level for
         * this exposure. The correction scales a pinned value ABOVE the
         * ceiling, so testing the calibrated number would miss every clip. */
        bool pinned = (v >= sat);
        if (pinned) out->saturated |= (u8)(1u << ch);

        /* Per-part correction. Off (or no OTP) leaves counts == raw, which is
         * what makes "lux setup <n> otp off" a real A/B comparison.
         *
         * ⚠️ A PINNED channel is not corrected, it is reported AT the ceiling.
         * The coefficient assumes a valid reading and scales it; applied to a
         * value that only means "at least full scale" it produces a count the
         * part cannot generate - seen on HW (on the old 16-bit scale) as
         * CLEAR=79870 and IR=71678 against a ceiling of 65535, which is exactly
         * 65535 x their own coefficients. Clamping keeps the row honest: the
         * number is the highest the sensor can report, and the SAT column says
         * it is a limit and not a measurement. */
        out->counts[ch] = pinned ? sat
                        : (out->otp_on
                            ? otp_apply(v, otp_calfactor(s->otp.gains[ch], gain))
                            : v);
    }

    /* No acknowledgment here: the caller drives the frame sequence through
     * vd_next_frame(), which acknowledges and then waits for completion. Doing
     * it here as well would acknowledge a frame that has only just started -
     * cutting it short and leaving the next read undefined. */
    return true;
}

/*
 * Switch channel 2 between the visible photodiodes and the covered ones.
 *
 * ⚠️ This is a MODE, not a per-sample measurement, and that distinction is the
 * whole point of this function.
 *
 * It used to be sampled: enter dark, discard one frame, read, leave again -
 * every single sample. That produced a "dark" that FOLLOWED THE LIGHT, and the
 * cause turned out to be simpler than the frame timing first blamed for it:
 * vd_configure() rewrote SEL_PD from SEL_PD_INIT on every "lux start", putting
 * channel 2 back on the visible diodes while the dark flag stayed set. The
 * register said 0x07 and the program believed 0x18.
 *
 * SEL_PD_x(1) = 0x18 is CORRECT, and measured on HW (sweep, gain 1x/400 ms).
 * ⚠️ The counts below are on the OLD 16-bit scale, before the port started
 * reading the low byte: multiply by ~256 to compare them with a reading taken
 * today. The ratios, which are what the table is for, are unaffected.
 *
 *     SEL_PD    dark      full light    ratio
 *     0x01         4          52295     x13000   visible diode
 *     0x02         4          41866     x10000   visible diode
 *     0x04         4          45580     x11000   visible diode
 *     0x08         1            166       x166   COVERED (D)
 *     0x10         1             91        x91   COVERED (D)
 *     0x18         1            120       x120   both covered ones
 *
 * Bits 3 and 4 sit at 0.3% of the visible diodes in full light and at 1 count
 * in the dark, which matches the datasheet's dark count noise spec (0..1.484
 * counts). They are the two black squares of Figure 17 (p.41) - the legend
 * lists "D = dark channel" but the figure prints the D black on black, which
 * is why they look unlabelled.
 *
 * The bits index the photodiodes of the channel's own colour, and the count is
 * NORMALISED over how many are selected (SPARE_1 = 0x3f, ST's "set count to be
 * independent of pd numbers"). That is why 0x1F reads LOWER than 0x07: adding
 * the two dark diodes to three lit ones pulls the average down, it does not
 * add to it.
 *
 * So: this only flips the mode and lets the caller discard the frame in
 * flight. While dark is on, CH2 carries dark and there is NO visible reading -
 * they share the channel, which is a property of the part, not a limitation
 * of this port.
 */
bool vd_set_dark_mode(sensor_ctx_t *s, bool on)
{
    const u8 want = on ? SEL_PD1_DARK : SEL_PD1_VISIBLE;
    u8 back = 0;

    if (!vd_write_reg(s->bus, VD6283_SEL_PD(1), want)) return false;

    /* Read it back: a silently ignored write would leave the channel on the
     * wrong photodiodes, and every later reading would be mislabelled -
     * "dark" that is really visible, or the reverse. */
    if (!vd_read_regs(s->bus, VD6283_SEL_PD(1), &back, 1u) || back != want) {
        (void)vd_write_reg(s->bus, VD6283_SEL_PD(1), SEL_PD1_VISIBLE);
        return false;
    }

    s->dark_mode = on;
    return true;
}

/*
 * Push the current gain and exposure to the part.
 *
 * ⚠️ The ALS engine is STOPPED around the writes, and that is not optional.
 * The ST driver refuses both operations on a running acquisition:
 * STALS_SetGain checks CHECK_CHANNEL_NOT_IN_USE against dev->als.chan, which
 * start_als sets and stop_als clears, and STALS_SetExposureTime checks
 * CHECH_DEV_ST_ALS_NOT_STARTED. So for ST these registers are written only
 * between Stop and Start.
 *
 * This function used to write them hot, with the engine running. It worked on
 * the bench - which is the problem with it: the part accepts the writes and
 * keeps producing plausible numbers, so nothing on the console distinguishes a
 * clean setting change from one latched halfway through a frame. Now that a
 * measurement is going to be built on top of the autogain (the dark refresh
 * forced by every gain move), being off the documented contract is no longer
 * an acceptable place to stand.
 *
 * ⚠️ ST's stop/start also toggles the channel enables (DC_EN / AC_EN, via
 * dev_disable_channels_for_mode). That is its own bookkeeping of which channels
 * belong to a running mode, not a latching requirement, so it is deliberately
 * NOT reproduced here - ALS_CTRL alone is the minimal change. If the counts
 * ever come out wrong after a gain step, toggling the channel enables in this
 * sequence is the next thing to try, and this is the note that says so.
 *
 * Gain and exposure are latched per measurement, so the frame in flight is
 * discarded by the caller either way.
 */
bool ag_apply(sensor_ctx_t *s)
{
    bool ok = true;

    /* Stop the state machine before touching its parameters. */
    if (!vd_write_reg(s->bus, VD6283_ALS_CTRL, 0x00u)) return false;

    if (!vd_write_reg(s->bus, VD6283_EXPOSURE_M + 1u, (u8)(s->exp_steps & 0xFFu)))
        ok = false;
    if (ok && !vd_write_reg(s->bus, VD6283_EXPOSURE_M,
                            (u8)((s->exp_steps >> 8) & 0x03u)))
        ok = false;
    for (u8 ch = 0; ok && ch < ALS_CHANNELS; ch++)
        if (!vd_write_reg(s->bus, (u8)(VD6283_GAIN_CH1 + ch), (u8)(s->gain_idx + 1u)))
            ok = false;

    /* ⚠️ Restart even when a write failed. Returning early here would leave the
     * part stopped with ALS_EN clear, and the measurement task would then sit
     * in vd_next_frame() timing out on every frame - a dead sensor produced by
     * the error path rather than by the fault. */
    if (!vd_write_reg(s->bus, VD6283_ALS_CTRL, ALS_CONT_MODE3 | ALS_EN)) ok = false;

    return ok;
}

/*
 * Measure the dark reference once, leaving channel 2 back on the visible
 * photodiodes. This is the "1 dark every N visible" half of the mixed
 * acquisition (see K_DARK_SCALE).
 *
 * ⚠️ COST. It is called with a completed frame pending and must return with a
 * completed VISIBLE frame pending, so the caller can read it as usual. That is
 * (1 + s->meas_discard) exposure windows per SEL_PD write, twice over - four
 * windows at the default, about 1.6 s at 400 ms. The count is settable at
 * runtime because it is an open question, not a tuned constant: see
 * MEAS_SWITCH_DISCARD.
 *
 * Only channel 2 is read: three bytes at VD6283_CH_DATA(1), rather than a whole
 * lux_sample_t built on the caller's stack for one value out of six.
 *
 * The count returned is RAW - no OTP coefficient. In "lux meas" the caller
 * subtracts it from the RAW visible count of the SAME channel at the same
 * gain, so the per-part factor is a multiplier common to both terms and is
 * applied once, afterwards, to the difference. In "lux meas_cl" it does not
 * cancel - signal and dark are then two different channels - and Kdark carries
 * the mismatch instead; see MEAS_CH_CLEAR.
 *
 * ⚠️ Note what does NOT happen in CLEAR mode: SEL_PD moves channel 2 only, so
 * channel 6 integrates undisturbed across the whole refresh. The discard
 * frames still cost their time, but they protect a channel the signal is no
 * longer read from - a straddled frame can corrupt the dark, never the clear.
 */
bool vd_measure_dark(sensor_ctx_t *s, u32 *out)
{
    u8   b[3] = { 0, 0, 0 };
    bool ok = true;

    if (!vd_set_dark_mode(s, true)) return false;

    /* Throw away the frames that may straddle the switch, then take the next. */
    for (u8 i = 0; ok && i < s->meas_discard; i++)
        if (!vd_next_frame(s)) ok = false;

    if (ok && !vd_next_frame(s))                                ok = false;
    if (ok && !vd_read_regs(s->bus, VD6283_CH_DATA(1), b, 3u))  ok = false;
    if (ok) *out = ((u32)b[0] << 16) | ((u32)b[1] << 8) | (u32)b[2];

    /* ⚠️ Always go back to the visible diodes, failure or not: leaving channel
     * 2 on the covered ones would turn every following VIS reading into a dark
     * one, silently, with the row still labelled VIS. That is the single worst
     * way this function can fail, so the return is not on the success path. */
    if (!vd_set_dark_mode(s, false)) return false;

    for (u8 i = 0; i < s->meas_discard; i++)
        if (!vd_next_frame(s)) ok = false;

    if (!vd_next_frame(s)) ok = false;      /* leave a clean visible frame ready */

    return ok;
}

/* ⚠️ PROBE_COUNT in the header must match this table: the CLI uses it to state
 * how long the sweep takes. The check below makes a mismatch a compile error
 * instead of a wrong number on the console. */
static const u8 PROBE_VALUES[] = {
    0x01u, 0x02u, 0x04u, 0x08u, 0x10u,  /* one bit at a time */
    0x07u, 0x0Fu, 0x1Fu,                /* the values ST uses */
    0x18u,                              /* the "dark" value that failed */
    0x20u, 0x40u, 0x80u,                /* beyond 5 bits: do they even stick? */
};

_Static_assert(sizeof(PROBE_VALUES) / sizeof(PROBE_VALUES[0]) == PROBE_COUNT,
               "PROBE_COUNT must match PROBE_VALUES");

void vd_probe_sweep(sensor_ctx_t *s, u8 ch)
{
    u8 saved = 0;

    if (ch >= ALS_CHANNELS) return;

    /* Keep the current selection so the sweep leaves no trace. */
    if (!vd_read_regs(s->bus, VD6283_SEL_PD(ch), &saved, 1u)) {
        ca_stdout_printf_n("probe: cannot read SEL_PD(%u) - aborted", (u32)ch);
        return;
    }

    ca_stdout_printf_n("");
    ca_stdout_printf_n("=== SEL_PD sweep: sensor %s channel %u (%s), reg 0x%02X ===",
                       s->label, (u32)ch, CH_NAME[ch], VD6283_SEL_PD(ch));
    ca_stdout_printf_n("  current value 0x%02X, gain %u.%02ux, exposure %u ms",
                       saved, (u32)(GAIN_VALUE[s->gain_idx] >> 8),
                       (u32)(((GAIN_VALUE[s->gain_idx] & 0xFFu) * 100u) >> 8),
                       exposure_ms(s));
    ca_stdout_printf_n("  write  read  count      note");

    for (u32 i = 0; i < PROBE_COUNT; i++) {
        u8  want = PROBE_VALUES[i], back = 0;
        u8  raw[3];
        u32 v = 0;

        if (!vd_write_reg(s->bus, VD6283_SEL_PD(ch), want)) {
            ca_stdout_printf_n("   0x%02X    --     write failed", want);
            continue;
        }
        /* Read it back: a value the register truncates tells us how wide it
         * really is, which is half of what this sweep is for. */
        if (!vd_read_regs(s->bus, VD6283_SEL_PD(ch), &back, 1u)) back = 0xFFu;

        /* The write landed inside a running frame, so that one is a mix:
         * discard it and measure the next, which integrates entirely on the
         * new selection. Same reason the dark read needed two frames. */
        if (!vd_next_frame(s)) { ca_stdout_printf_n("   0x%02X   0x%02X   (no frame)", want, back); continue; }
        if (!vd_next_frame(s)) { ca_stdout_printf_n("   0x%02X   0x%02X   (no frame)", want, back); continue; }

        if (vd_read_regs(s->bus, (u8)VD6283_CH_DATA(ch), raw, 3u))
            v = ((u32)raw[0] << 16) | ((u32)raw[1] << 8) | (u32)raw[2];

        ca_stdout_printf_n("   0x%02X   0x%02X  %8u   %s%s", want, back, v,
                           (back != want) ? "truncated by the register" : "",
                           (v == 0u) ? "  no diode selected?" : "");
    }

    /* Put it back and let one frame settle on the restored selection.
     * For channel 2 restore the MODE rather than the byte read at entry:
     * if the two ever disagree, the flag is what the rest of the program
     * believes, and leaving the register on the other value is what made a
     * "dark mode" sample read the visible diodes. */
    u8 restore = (ch == 1u) ? (s->dark_mode ? SEL_PD1_DARK : SEL_PD1_VISIBLE)
                            : saved;
    (void)vd_write_reg(s->bus, VD6283_SEL_PD(ch), restore);
    (void)vd_next_frame(s);

    ca_stdout_printf_n("  restored 0x%02X%s", restore,
                       (restore != saved) ? "  (mode, not the value found)" : "");
    ca_stdout_printf_n("  Run this twice - once lit, once covered. A bit that");
    ca_stdout_printf_n("  selects a COVERED diode reads low and does NOT change.");
    ca_stdout_printf_n("");
}

/*
 * Dump the four bytes of every channel group, unassembled.
 *
 * Each channel occupies 0x05+4c .. 0x08+4c: a padding byte, then H, M, L
 * (§4.3.6). This deliberately starts one byte EARLY, at the padding, so the
 * assumption vd_read_channels() rests on stays checkable rather than being
 * taken on trust from a comment.
 *
 * HML is what the application reports; HM is the 16-bit format it used to
 * report, kept alongside so the resolution the low byte carries is visible in
 * the same frame. PAD must be 0 on every line - if it is not, the base address
 * is wrong and nothing downstream can be trusted.
 */
void vd_dump_bytes(sensor_ctx_t *s)
{
    u8  pad_seen = 0;       /* OR of every pad byte: expected to stay 0 */
    u32 low_seen = 0;       /* how many channel reads had a non-zero low byte */
    u32 reads    = 0;

    ca_stdout_printf_n("");
    ca_stdout_printf_n("=== raw channel bytes: sensor %s, gain %u.%02ux, exposure %u ms ===",
                       s->label, (u32)(GAIN_VALUE[s->gain_idx] >> 8),
                       (u32)(((GAIN_VALUE[s->gain_idx] & 0xFFu) * 100u) >> 8),
                       exposure_ms(s));
    ca_stdout_printf_n("  reg 0x05+4c .. 0x08+4c, %u frames. PAD must be 00.",
                       BYTES_FRAMES);
    ca_stdout_printf_n("  HML = what is reported (24-bit); HM = the old 16-bit reading.");
    ca_stdout_printf_n("  frame ch name   PAD  H   M   L        HM       HML");

    for (u32 f = 0; f < BYTES_FRAMES; f++) {
        u8 raw[ALS_CHANNELS * 4u];

        /* One clean frame per line-set, the same handshake the sampler uses:
         * without it the group below could straddle a frame boundary and the
         * low byte would look noisy for a reason that is not the sensor's. */
        if (!vd_next_frame(s)) {
            ca_stdout_printf_n("   %u    (no frame)", f);
            continue;
        }
        if (!vd_read_regs(s->bus, (u8)VD6283_CH_PAD(0), raw, (u16)sizeof(raw))) {
            ca_stdout_printf_n("   %u    (read failed)", f);
            continue;
        }

        for (u8 ch = 0; ch < ALS_CHANNELS; ch++) {
            const u8 *p = &raw[ch * 4u];
            u32 hm  = ((u32)p[1] << 8)  | (u32)p[2];
            u32 hml = ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];

            pad_seen |= p[0];
            if (p[3]) low_seen++;
            reads++;

            ca_stdout_printf_n("   %u    %u  %-5s  %02X  %02X  %02X  %02X  %8u  %8u",
                               f, (u32)ch, CH_NAME[ch],
                               p[0], p[1], p[2], p[3], hm, hml);
        }
    }

    ca_stdout_printf_n("");
    if (pad_seen)
        ca_stdout_printf_n("  PAD was NOT always zero (saw bits %02X) - 0x05+4c carries"
                           " data, and the byte split has to be re-derived.", pad_seen);
    else
        ca_stdout_printf_n("  PAD zero on all %u reads: 0x05+4c is padding, as assumed.",
                           reads);

    if (low_seen)
        ca_stdout_printf_n("  LOW byte non-zero on %u of %u reads: the 8 bits HM would"
                           " throw away are carrying signal.", low_seen, reads);
    else
        ca_stdout_printf_n("  LOW byte zero on all %u reads: HM and HML agree, nothing"
                           " is being gained at these settings.", reads);

    ca_stdout_printf_n("  Run this lit AND with the sensor covered - the dark channel is"
                       " where the low byte matters most.");
    ca_stdout_printf_n("");
}

#endif /* EN_I2C */
