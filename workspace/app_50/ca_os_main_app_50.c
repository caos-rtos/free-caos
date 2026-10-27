// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * app_50 (lux_sensor) — two VD6283TX ambient light sensors, one per I2C bus
 *
 * Two measurement tasks (one per bus) read all 6 ALS channels and push the
 * samples into a shared static queue; a third task drains the queue and prints
 * them. The queue is 8 items deep, so it absorbs a full print stall without
 * losing a sample.
 *
 * The two measurement tasks start IDLE, polling a run flag: "lux start" brings
 * both up, "lux start 1" or "lux start 2" one at a time - which is what to use
 * when a bus is suspect, since with both free-running from reset there is no
 * telling which one a failure belongs to. Both start and stop only set that
 * flag - the task acts on it at a frame boundary, so the CLI never interrupts
 * an I2C transaction and never suspends a task from outside.
 *
 * ── The sensor ──────────────────────────────────────────────────────────────
 * VD6283TX (DS13735 rev 3, doc/reference_manuals/vd6283tx.pdf).
 * 6-channel colour sensor: red, visible, blue, green, IR, clear. 24-bit ADC.
 *
 * DARK — the matrix also holds two COVERED photodiodes. They are not a seventh
 * channel: the ST driver switches channel 2 onto them (SEL_PD_x(1) = 0x18) and
 * reads the count from ALS_CH2_DATA, so dark and visible cannot be sampled in
 * the same frame - and, being a MODE in the ST driver, dark is not meant to be
 * switched in and out per sample either. "lux dark <n> on" enters it and it
 * stays until switched off.
 *
 * The value is a zero REFERENCE, not an offset to subtract: every channel
 * already has its dark level removed in the analog chain (DC_CLAMP_EN = 0x1f
 * for CH1..CH5, AC_CLAMP_EN = 1 for CH6/clear, which sits on the AC path
 * because it doubles as the flicker channel). Being covered, it must NOT
 * follow the light: if it does, the mode did not take.
 *
 * ⚠️ ADDRESS — 0x40 and 0x20 are the SAME address, written two ways. §3.4.1:
 * "a slave address of 0x40 (or 0x20 in 7 bits)". 0x40 is the 8-bit write byte
 * (address<<1 | R/W=0), 0x20 the 7-bit address; Figure 9 shows the byte as
 * 0100000+R/W. The caOS API takes the 7-BIT form (ca_i2c_hw_transfer7 shifts
 * it itself), so VD6283_ADDR below is 0x20 and that is correct.
 *
 * The address is fixed for every VD6283TX: changing it means holding GPIO1 low
 * across power-up, repeated at every power-up (§3.4.2). That is why the two
 * sensors live on two SEPARATE buses - on one bus they would both answer.
 *
 * ── Wiring (2 sensors, 4 wires + 2 pull-ups per bus) ────────────────────────
 * Each sensor needs SCL, SDA, VDD (2v8/1v8) and GND, plus 4.7k pull-ups to
 * VDD on SCL and SDA. Run "wire" first on a new board: it drives the pins as
 * plain GPIO, so it tells a wiring fault apart from a peripheral one.
 * The pins are printed by "show" for the board actually running.
 *
 * ── Low-noise configuration (§3.2, §3.3.3, §3.3.4) ─────────────────────────
 * Every knob the part offers is set for the quietest reading:
 *   EXTIME  = 400 ms          — long enough to average out shot noise and to
 *                               stay above the 0x047-step threshold of Table
 *                               16, which is what keeps the saturation ceiling
 *                               at full scale instead of per-step. The maximum
 *                               (1.6384 s) is quieter but makes a 7-value
 *                               sample take ~1.6 s; see SAMPLE_PERIOD_MS.
 *   24-bit    readout         — the low byte matters at low light (§3.3.3);
 *                               we read all three bytes, not the top 16 bits.
 *   AGAIN   = 1x per channel  — unity gain. Analog gain amplifies the noise
 *                               along with the signal; the exposure already
 *                               provides the integration.
 *   ALS_PERIOD = 0            — back-to-back measurements, no idle gap. The
 *                               pace is set by EXTIME alone. Raised with
 *                               "lux period" when the point is to sample
 *                               rarely rather than to sample well.
 *   continuous mode           — free-running; no start/stop settling per frame.
 *
 * One exposure is 398 ms plus the ~6 ms conversion housekeeping (§3.2), plus
 * whatever idle gap ALS_PERIOD adds, and a sample is ONE of them: all six
 * channels come from a single frame.  (398, not 400: EXTIME x 1.6 ms with no
 * +1 - see EXTIME_PLUS_ONE, the datasheet formula contradicts its own tables.)
 *
 * DARK is a MODE, not a seventh value - "lux dark <n> on" moves channel 2 onto
 * the covered photodiodes and leaves it there, so while it is on there is no
 * visible reading. Sampling it per frame (enter dark, discard, read, leave)
 * was tried and produced a dark that FOLLOWED THE LIGHT: the I2C write is not
 * synchronised to the frame boundary, so the window read was part visible.
 *
 * ── CLI ────────────────────────────────────────────────────────────────────
 *   lux start <1|2>  — release one measurement task
 *   lux stop  <1|2>  — park it again and disable that sensor's ALS engine
 *   lux dark <n> on  — move channel 2 onto the covered photodiodes (a MODE)
 *   lux otp <1|2>    — the factory calibration read from that part
 *   lux setup <1|2>  — interactive menu (exposure, gain, otp, autogain, dark)
 *   lux meas <d> [n] — mixed acquisition: VIS corrected by a held dark. Kdark
 *                      is NOT an argument: it is per-part, see "lux kdark"
 *   lux meas_cl ...  — the same on CLEAR (ch6): 5 photodiodes, no filter,
 *                      the widest band the part has
 *   lux cal          — measure k = VIS/(R+G+B). NEEDS A LIT BENCH, and it is
 *                      the only thing that measures k: "lux start" does not
 *   lux kdark        — the per-PART dark coefficient: show, set, or "cal" to
 *                      measure it, which needs the opposite (a closed box)
 *   lux lx           — lux and CCT from R,G,B (ST's matrix, uncalibrated)
 *   lux period <ms>  — idle gap between measurements, for an always-on alarm
 *   map              — sensor-to-connector wiring map (also printed at boot)
 *   scan             — probe every I2C address: wiring fault vs wrong address
 *   reg <a> [v]      — raw register read/write in hex, for undocumented bits
 *   show             — sensor status, configuration and per-bus counters
 *   wire             — continuity test on the I2C pins (GPIO only, tasks idle)
 *   raw              — one immediate read, bypassing the queue
 *   help             — command list
 *
 * LEDs: LED1 toggles on every sample from bus A, LED2 on every sample from
 *       bus B. One LED stuck means that sensor stopped answering.
 *
 * ⚠️ AND THEY ARE OFF (EN_LED=0), DELIBERATELY. This is a photometer, and on
 * this bench the board sits in the same closed box as the sensors: an LED that
 * toggles once per sample is a light source switching in antiphase with the
 * measurement, and the sensor sees it.
 *
 * Measured 2026-09-02, "lux meas_cl" at 1.00x/398 ms, both sensors, 175
 * samples: NCLR alternated on the PARITY of the sample index, and stayed
 * locked to it for the whole run.
 *
 *              level      odd/even delta
 *   B open      6796           1161
 *   B in box    3036           1075     level -55%, delta -7%
 *
 * The contribution is ADDITIVE - a constant added light - where a fault in
 * gain, exposure or frame handshaking would have scaled with the light. The
 * detail that settles it: the rows marked "D" read HIGHER than their even
 * neighbours (A: 11304 against ~10675), because a dark refresh runs four
 * frames that queue no sample and therefore do not toggle the LED - the window
 * right after it integrates with the LED held steady. Nothing else ties the
 * amplitude of the alternation to the number of frames skipped.
 *
 * So: turning the LEDs back on turns a ~20-35% square wave back on top of
 * every reading. If a liveness indicator is needed during a capture, it has to
 * be something the sensor cannot see - a counter on the console, not a diode
 * in the box. EN_CLI_LED went off with it: led.c is entirely inside #if EN_LED,
 * so the "led" CLI command was compiled out anyway and the flag only lied.
 */

#include "caos.h"
#include "vd6283_ctrl.h"
#include "vd6283_i2c.h"

/* No MCU_ID here, by design.
 *
 * This file used to declare ca_i2c_hw_last_error() extern behind
 * `#if MCU_ID == 575 || 753 || 563`, reaching into
 * hw/<family>/drivers/i2c_<family>.c for a function that is not public API.
 * The guard existed because the function is missing on the other families -
 * which is exactly the sign that the app was in the wrong place. An
 * application talks to caOS through its API and neither knows nor cares which
 * part is underneath.
 *
 * The diagnostic decoded the I2C ISR to separate "nobody answered at this
 * address" from "the lines never went idle" (missing pull-ups). Deliberately
 * NOT replaced by an API: at application level "the sensor did not answer" is
 * the whole of what the app can act on, and it already reports that. Why it
 * did not answer is an investigation, and an investigation wants an
 * instrument - a scope, or `i2c` from the console - not one more field in a
 * status line.
 *
 * Board wiring is a different matter and stays in vd6283_ctrl.h: connector
 * numbers and which I2C bus the sensor is soldered to are facts about the
 * board, not about the part, and no API can abstract a jumper.
 */

#if EN_I2C


/* ============================================================================
 * MODULE STATE — owned by this file
 *
 * The sensor table itself lives in vd6283_ctrl.c, because every unit needs it.
 * What follows is only used here: the sample queue and two counters.
 * ========================================================================== */

/* Union-based definition: the macro sizes the storage and pre-initialises the
 * header, and access goes through .q so the type pun stays legal. Never cast
 * the raw buffer to ca_queue_static_t* - that is strict-aliasing UB, and this
 * project has already been bitten by it once in a -O3 Release build. */
CAQUEUE_STATIC_DEFINE(s_queue, Q_DEPTH, sizeof(lux_sample_t));

static volatile u32 s_printed = 0;

/* Set by the first sensor to start, so the column header is printed once and
 * not once per task. Cleared when every sensor has stopped, so a new run gets
 * a fresh header. */
static volatile bool s_header_done = false;

/* ============================================================================
 * SENSOR ACCESS
 * ========================================================================== */



/* ============================================================================
 * OTP — per-part factory calibration
 *
 * Ported from otp_reset() / opt_read_init() / otp_read() of the ST driver.
 * The 120 bits live in two 8-byte banks that are NOT plain little-endian: every
 * byte is bit-reversed, and bank 1 is offset by 4 bits because the low nibble
 * of its first byte belongs to bank 0. Getting the packing wrong yields
 * plausible-looking coefficients that are simply the wrong bits, so this
 * follows the driver literally rather than tidying it up.
 * ========================================================================== */












/* ============================================================================
 * AUTOGAIN
 *
 * ⚠️ The ST driver has no ALS autogain to borrow: STALS_ALS_AUTOGAIN_CTRL
 * returns STALS_ERROR_FNCT_DEPRECATED, and STALS_LIB_flk_autogain drives the
 * FLICKER path (a binary search on AC_SAT_METRIC), not this one. So this is
 * ours.
 *
 * Policy: ONE gain for all six channels, steered by clear. Clear has five
 * photodiodes and no filter, so it is the most sensitive and clips first -
 * holding it in range keeps the others in range too. A single shared gain also
 * keeps the six comparable with each other, so a difference like clear - vis
 * (the IR content) stays meaningful without renormalising.
 *
 * Gain first, exposure second: gain spans 0.71x..66.67x without changing how
 * long a sample takes, while exposure changes the pace by the same factor. The
 * exposure only moves once the gain is at the end of its travel. Combined
 * dynamic range is about 96000x.
 *
 * One step per sample, never a jump, with a dead band between the thresholds:
 * without the gap a reading sitting near a single threshold would oscillate
 * between two gains forever.
 * ========================================================================== */


/* ── The clear-to-RGB ratio ─────────────────────────────────────────────────
 *
 * When clear is pinned at saturation it carries NO information about how far
 * over it is - every over-exposure reads the same ceiling, so the ratio needed
 * to correct is unknowable from clear alone. Red, green and blue are 3-diode
 * filtered channels that read far lower, so they are still on scale when clear
 * has gone: their mean, scaled up, estimates what clear WOULD have read.
 *
 * ⚠️ The scale factor is NOT a constant. Measured on HW across two lighting
 * conditions it ranged from 8.4 to 24.6 - clear has no filter and sees IR,
 * so the ratio follows the SPECTRUM of the light, not the sensor. A fixed
 * constant would be wrong by up to 3x.
 *
 * So it is measured: every unsaturated sample updates it, and the stored value
 * is used while clear is pinned. K_TYPICAL only seeds it for the very first
 * sample, when nothing has been measured yet.
 *
 * Held x256 to keep the arithmetic integer; AG_K_SCALE and AG_K_TYPICAL are
 * defined with the other tuning constants near the top, because the sensor
 * table initialises ag_k. */







/* ============================================================================
 * SEL_PD SWEEP — what does each bit actually select?
 *
 * SEL_PD (0x6B+c) is not in the datasheet, and the ST driver writes it without
 * saying what the bits mean. What IS known, from select_pd() cross-checked
 * against the photodiode matrix (Figure 17, p.41):
 *
 *     channel   SEL_PD   bits set   diodes of that colour in the matrix
 *     red       0x07        3                3
 *     vis       0x07        3                3
 *     blue      0x07        3                3
 *     green     0x1F        5                5
 *     IR        0x0F        4                4
 *     clear     0x1F        5                5
 *
 * Six for six: the number of bits set equals the number of photodiodes of that
 * channel's colour. So the bits appear to index the diodes OF THE CHANNEL'S
 * OWN COLOUR, not absolute positions in the matrix - which is why 0x18 (bits 3
 * and 4) on channel 2 does nothing useful: vis has only 3 diodes, so those two
 * bits are out of range and the channel keeps reading visible light. That
 * matches the measurement: "dark" tracked the light exactly.
 *
 * This sweep tests that reading. It walks a set of SEL_PD values on one
 * channel, takes a clean frame for each, and prints the count. Run it while
 * the light is steady, then run it again with the sensor covered: a bit that
 * selects a COVERED diode gives a low count that does NOT change between the
 * two runs, which is the signature we are looking for.
 *
 * It runs in the measurement task, which owns the bus - the CLI only asks.
 * ========================================================================== */



/* Defined with the printing code below; the measurement task emits the header
 * when a sensor starts, which is before that point in the file. */
static void print_header(void);

/* ============================================================================
 * MEASUREMENT TASKS — one per sensor, one per bus
 * ========================================================================== */

/* Put back everything "lux kdark cal" borrowed, and park the sensor if the
 * command is what started it.
 *
 * ⚠️ A macro, and used at BOTH exits - the one that stores a coefficient and
 * the one that gives up. The give-up path is the one that matters: it is
 * reached when the dark reads 0 or a channel is pinned, and forgetting the
 * restore there would leave the sensor at maximum gain and 1636 ms exposure
 * with the operator's own settings silently gone, after a command that had
 * just announced it had failed. */
#define KDARK_RESTORE(s)                                        \
    do {                                                        \
        (s)->dark_req  = (s)->kd_dark_was;                      \
        (s)->gain_idx  = (s)->kd_gain_was;                      \
        (s)->exp_steps = (s)->kd_exp_was;                       \
        (s)->auto_gain = (s)->kd_auto_was;                      \
        if ((s)->kd_stop) { (s)->kd_stop = false;               \
                            (s)->running = false; }             \
    } while (0)

static u32 task_measure(void *arg)
{
    const u32 idx = (u32)(uintptr_t)arg;
    /* ⚠️ The FIRST argument is the scheduler's thread id, NOT the parameter
     * passed to ca_os_job - that one arrives as the SECOND. See
     * ca_thread_launch_thread: `u32 idx = ca_thread_start_task(); f(idx, p1)`.
     * Indexing s_sensor[] with the thread id put `s` outside the array from
     * the very first run, because stdout and the CLI are created first and
     * these tasks never get thid 0 and 1. The thread id also CHANGES when a
     * task is removed, so it is never a valid array index. */
    if (idx >= SENSOR_COUNT) return 0;      /* never index out of the array */
    sensor_ctx_t *s = &s_sensor[idx];
    lux_sample_t  sample;
    u32           seq = 0;

    /* The task gates itself on a plain flag polled every IDLE_POLL_MS, and
     * nothing ever suspends it from the outside: it always stops at a point it
     * chose, so it can never be frozen mid-transaction with the I2C bus held.
     *
     * Why not a synchronisation primitive:
     *  - a mutex cannot work at all here — ca_mutex_unlock enforces ownership
     *    (CA_SAFEGUARD on owner_tid), so the CLI could not release what the
     *    measurement task locked;
     *  - a semaphore has no owner, but ca_sem_wait assumes the resource is
     *    available on return without re-checking, so the handshake depends on
     *    the task having reached the wait BEFORE the post. Measured on HW: a
     *    "lux start" issued from the CLI left woke=0, i.e. the task never came
     *    back from the wait.
     * A polled flag has no such ordering requirement: the start can happen at
     * any time, even before the task first runs, and it is picked up at the
     * next poll. It costs one wake-up per poll interval on an idle task,
     * nothing next to a 1.6 s exposure.
     *
     * The outer loop also means a restart reconfigures the part: "lux stop"
     * disables the ALS engine, and resuming without vd_configure would read a
     * stopped sensor. */
    while (1) {
        while (!s->running) ca_os_sleep(IDLE_POLL_MS);

        if (!s->up) {
            ca_stdout_printf_n("lux: bus %s (I2C%u) did not initialise",
                               s->label, s->bus);
            s->running = false;
            continue;
        }

        /* Trace the bring-up: with only the end result visible it is
         * impossible to tell "the task never got here" from "the transfer
         * silently did nothing", and those need opposite investigations. */
        s->woke++;
        ca_stdout_printf_n("lux %s: woke up, probing I2C%u...", s->label, s->bus);

        s->present = vd_configure(s);
        if (!s->present) {
            ca_stdout_printf_n("lux: no VD6283TX on bus %s (I2C%u) - id=0x%02X exp 0x%02X",
                               s->label, s->bus, s->dev_id, VD6283_DEV_ID_VAL);
            s->running = false;
            continue;
        }

        ca_stdout_printf_n("lux: sensor %s ready on I2C%u (id=0x%02X rev=0x%02X)",
                           s->label, s->bus, s->dev_id, s->rev_id);

        /* The configuration that produced the rows below, then the column
         * names. A capture that starts mid-stream is not interpretable: gain,
         * exposure and the OTP state all scale the numbers. */
        {
            u32 gw, gf; gain_text(s->gain_idx, &gw, &gf);
            /* clamp belongs in the record: with it off, channel 2 is running
             * outside the ST driver's configuration and every dark number in
             * the capture means something different. */
            ca_stdout_printf_n("# sensor %s  I2C%u  gain=%u.%02ux  exp=%ums"
                               "  period=%ums"
                               "  otp=%s  dark=%s  autogain=%s  ch2clamp=%s",
                               s->label, s->bus, gw, gf, exposure_ms(s),
                               IM_STEPS_TO_MS(s->im_steps),
                               !s->otp.read ? "n/a" : s->otp_on ? "on" : "off",
                               s->dark_mode ? "on" : "off",
                               s->auto_gain ? "on" : "off",
                               s->ch2_clamp ? "on" : "OFF");
            /* k lives here rather than in a column: "lux cal" freezes it, so it
             * is the same on every row of a capture. Printing it once per
             * sensor records what produced the VIS column without spending six
             * characters of a line that has none to spare. */
            /* In the mixed acquisition k is not what produced the numbers -
             * Kdark and the dark cadence are. Printing k here would name the
             * wrong coefficient in the record of the run. */
            if (s->meas_on) {
                /* discard is part of the record, not a detail: it is the
                 * variable of an open experiment, and a capture that does not
                 * say which value produced it cannot be compared with the
                 * next one. */
                const char *mn = (s->meas_ch == MEAS_CH_CLEAR) ? "CLEAR" : "VIS";
                ca_stdout_printf_n("# saturation=%u counts  cadence~%ums/sample"
                                   "  meas on %s: 1 dark every %u  Kdark=%u.%03u"
                                   "  discard=%u",
                                   als_sat_level(s->exp_steps),
                                   SAMPLE_PERIOD_MS(s), mn, (u32)s->meas_div,
                                   s->k_dark / K_DARK_SCALE,
                                   s->k_dark % K_DARK_SCALE,
                                   (u32)s->meas_discard);
                /* ⚠️ Says which channel, because Kdark means a different thing
                 * on each: on VIS signal and dark share channel 2 and the OTP
                 * factor cancels, on CLEAR they do not. A capture that only
                 * recorded the number could not be re-read later. */
                /* ⚠️ THREE lines, not one long one, and not two. Each has to
                 * stay under the 128 characters ca_stdout_printf_n can emit
                 * with its CRLF intact - past that the line loses the ending
                 * and the NEXT banner line is pasted onto its tail, then
                 * truncated with "~OVF". Produced exactly that on HW when this
                 * text first grew: the SIGNED note and the "!" note came out
                 * fused into one unreadable row. The row builder has a
                 * _Static_assert for this; these prose lines have only the
                 * author, so they are kept visibly short. */
                ca_stdout_printf_n("# N%s = norm(%s - Kdark*dark), on raw counts"
                                   " before normalising",
                                   (s->meas_ch == MEAS_CH_CLEAR) ? "CLR" : "VIS",
                                   mn);
                ca_stdout_printf_n("#   SIGNED: at the floor it is noise about"
                                   " zero, half of it negative");
                ca_stdout_printf_n("#   \"!\" = Kdark out of calibration"
                                   " (residue >%u%% below zero), not merely"
                                   " negative", MEAS_MISCAL_PCT);
                /* The lux column comes from a different quantity by a different
                 * route - R,G,B through a matrix, not one channel minus a dark -
                 * so the capture has to say so, and has to say that its absolute
                 * scale is not calibrated on this bench. */
                ca_stdout_printf_n("# mLUX = millilux from R,G,B (ST matrix,"
                                   " ABSOLUTE SCALE UNCALIBRATED), independent"
                                   " of Kdark and of the dark");
            } else if (s->cal_k != 0u) {
                u32 km = (u32)(((u64)s->cal_k * 1000u) / CAL_K_SCALE);
                ca_stdout_printf_n("# saturation=%u counts  cadence~%ums/sample"
                                   "  k=%u.%03u",
                                   als_sat_level(s->exp_steps),
                                   SAMPLE_PERIOD_MS(s), km / 1000u, km % 1000u);
            } else {
                ca_stdout_printf_n("# saturation=%u counts  cadence~%ums/sample"
                                   "  k not measured yet",
                                   als_sat_level(s->exp_steps),
                                   SAMPLE_PERIOD_MS(s));
            }
        }

        /* The column header goes out ONCE, from whichever sensor starts first:
         * both tasks run this, and two identical headers in the capture make
         * the first data row look like a header too. The "#" line above is
         * per-sensor on purpose - their settings can differ. */
        if (!s_header_done) {
            s_header_done = true;
            /* The first rows of a fresh run are the calibration ones: say so,
             * or the CAL marker and the missing CVIS look like a fault. */
            if (s->cal_req > 0u)
                ca_stdout_printf_n(
                    "# first %u rows are marked CAL: measuring k = VIS/(R+G+B),"
                    " CVIS fills in after", CAL_SAMPLES);
            print_header();
        }

        /* The first frame after enabling is already integrating: wait for it
         * to complete before trusting a readout. */
        if (!vd_next_frame(s)) {
            ca_stdout_printf_n("lux: sensor %s produced no first frame", s->label);
            s->running = false;
            continue;
        }

        while (s->running) {
            /* ── Mixed acquisition: refresh the held dark ────────────────────
             * Done HERE, at the top, with a completed frame pending and before
             * the sample is read, so the dark this row is corrected with is
             * either brand new or at most meas_div samples old - never one
             * taken AFTER the reading it corrects.
             *
             * Three reasons to refresh: the cadence elapsed, nothing has been
             * measured yet, or gain/exposure moved. The last one is what makes
             * the subtraction legal before normalisation - see K_DARK_SCALE. */
            if (s->meas_on &&
                (s->dark_forced || !s->dark_hold_ok ||
                 s->vis_since_dark >= s->meas_div)) {
                u32 d = 0;
                /* ⚠️ A dark of zero is STORED, not rejected. At 1.00x/400 ms
                 * the floor of these parts is below one count - measured under
                 * full cover, ten rows, both sensors - so zero is the correct
                 * reading in darkness and filtering it would silently substitute
                 * a stale value from a lit moment. See the note by the
                 * correction below. */
                if (vd_measure_dark(s, &d)) {
                    s->dark_hold      = d;
                    s->dark_hold_ok   = true;
                    s->dark_was_fresh = true;   /* marks the row about to be read */
                    s->last_dark      = d;
                } else {
                    s->errors++;
                    /* Leave dark_hold_ok as it was: an old dark is a worse
                     * answer than a fresh one but a far better one than none,
                     * and dropping it would blank the column on a single failed
                     * transaction. */
                }
                s->dark_forced    = false;
                s->vis_since_dark = 0;

                /* A stop during the four windows above must not be ignored:
                 * that is up to 1.6 s in which "lux stop" would look dead. */
                if (!s->running) break;
            }

            if (vd_read_channels(s, &sample)) {
                sample.seq    = ++seq;
                sample.bus    = s->bus;
                sample.sensor = (u8)idx;
                /* Snapshot the loss counter INTO the sample: print_sample runs
                 * in the printing task, and reading s_sensor[] from there would
                 * race the measurement task that owns it. */
                sample.dropped = s->dropped;

                /* ── VIS reconstruction ─────────────────────────────────────
                 * Uses the OTP-CORRECTED counts, not the raw ones. k is meant
                 * to be a property of the light and of the nominal photodiode
                 * responses, and that is exactly what the per-part correction
                 * removes: in raw space
                 *     k_raw = (vis/c_vis) / (r/c_r + g/c_g + b/c_b)
                 * carries FOUR different per-part coefficients, so it differs
                 * between two parts under identical light — the very thing we
                 * want k not to do.
                 *
                 * ⚠️ An earlier comment here claimed raw and corrected "scale
                 * together, so applying k to raw and correcting afterwards is
                 * equivalent". That holds only if the four coefficients are
                 * equal, which is precisely what the OTP denies: the sum is
                 * multi-channel, so the correction does NOT factor out of it.
                 * (The dark subtraction further down IS allowed to work on raw
                 * counts: VIS and DARK share one channel and therefore one
                 * coefficient, and a single scalar does commute with it.)
                 *
                 * The correction is already inside counts[], so no otp_apply()
                 * is applied to the reconstruction afterwards. A pinned channel
                 * is reported clamped in counts[], but rgb_ok/cal_usable below
                 * refuse both the fit and the reconstruction in that case, so a
                 * clamped value never reaches this sum. With "otp off",
                 * counts[] == raw[] and this degrades to the old behaviour. */
                {
                    u32 rgb = sample.counts[0] + sample.counts[3] + sample.counts[2];

                    /* Mark the row while a calibration is in progress, so the
                     * samples that produced k are recognisable in the capture
                     * and can be filtered out of an analysis. */
                    sample.calibrating = (s->cal_req > 0u) ? 1u : 0u;

                    /* ⚠️ A pinned channel is not a measurement: it says "at
                     * least full scale" and nothing more. Averaging it into k
                     * would bias the coefficient, and with the autogain held
                     * off during calibration nothing would come along to fix
                     * the exposure. Such a sample is skipped WITHOUT
                     * decrementing cal_req, so the sixteen that make up k are
                     * sixteen valid ones. */
                    bool cal_usable = (sample.saturated & 0x0Fu) == 0u;

                    /* ⚠️ BOTH unusable cases are counted, and the budget is
                     * what makes "lux cal" terminate at all.
                     *
                     * rgb == 0 used to fall through BOTH branches: nothing was
                     * counted, cal_req was never decremented, and the
                     * calibration ran for ever - with the autogain held off
                     * below, so the sensor could not even climb out of it. That
                     * was reachable from a plain "lux start" until the
                     * automatic calibration was removed from lux_start_one();
                     * "lux cal" can still be typed in a dark box, and it must
                     * say so rather than hang. */
                    if (s->cal_req > 0u && !s->dark_mode &&
                        (!cal_usable || rgb == 0u)) {
                        s->cal_sat++;
                        if (s->cal_sat == 1u)
                            ca_stdout_printf_n(
                                "# sensor %s: %s during calibration", s->label,
                                !cal_usable
                                  ? "saturated - lower the gain or the light"
                                  : "R+G+B reads 0 - too dark to calibrate k");
                        if (s->cal_sat >= CAL_SKIP_MAX) {
                            ca_stdout_printf_n(
                                "# sensor %s: k ABANDONED after %u unusable"
                                " samples - k needs a steady LIT bench, it"
                                " cannot be measured in the dark",
                                s->label, s->cal_sat);
                            s->cal_req  = 0u;
                            s->dark_req = s->cal_dark_was;
                            if (s->cal_stop) {
                                s->cal_stop = false;
                                s->running  = false;
                            }
                        }
                    } else if (s->cal_req > 0u && !s->dark_mode && rgb > 0u) {
                        /* ⚠️ A pinned channel is not a measurement: it says
                         * "at least full scale" and nothing more. Averaging it
                         * into k would quietly bias the coefficient, and with
                         * the autogain held off during calibration nothing
                         * would come along to fix the exposure. Skip the
                         * sample instead - and do NOT decrement cal_req, so
                         * the sixteen that make up k are sixteen VALID ones. */
                        /* Calibrating: channel 2 still holds a MEASURED vis,
                         * so k can be computed for this sample.
                         * Corrected VIS over corrected RGB: both sides of the
                         * ratio must live in the same space, or k would carry
                         * the per-part coefficients it is meant to be free of. */
                        u32 k = (u32)(((u64)sample.counts[1] * CAL_K_SCALE) / rgb);
                        if (s->cal_n == 0u) { s->cal_min = k; s->cal_max = k; }
                        else {
                            if (k < s->cal_min) s->cal_min = k;
                            if (k > s->cal_max) s->cal_max = k;
                        }
                        s->cal_acc += k;
                        s->cal_n++;
                        s->cal_req--;

                        if (s->cal_req == 0u) {
                            s->cal_k    = s->cal_acc / s->cal_n;
                            s->cal_done = true;
                            /* Spread as per-mille of the mean: a wide one means
                             * the light was not steady and k is not usable. */
                            u32 spread = s->cal_k
                                ? (u32)(((u64)(s->cal_max - s->cal_min) * 1000u) / s->cal_k)
                                : 0u;
                            ca_stdout_printf_n(
                                "# sensor %s: k=%u.%03u  (spread %u.%u%%, %u samples%s)",
                                s->label, s->cal_k / CAL_K_SCALE,
                                (u32)(((u64)(s->cal_k % CAL_K_SCALE) * 1000u) / CAL_K_SCALE),
                                spread / 10u, spread % 10u, s->cal_n,
                                s->cal_sat ? ", some skipped" : "");
                            /* Put the dark setting back the way the user had
                             * it. Forcing dark on here would silently change
                             * the measurement the next start produces; the
                             * reconstruction works in either mode, so there is
                             * nothing to force. */
                            s->dark_req = s->cal_dark_was;

                            /* If "lux cal" started this sensor itself, park it
                             * again: calibration and acquisition are separate
                             * moments, and leaving it running would put the
                             * calibration transient at the head of the
                             * capture. */
                            if (s->cal_stop) {
                                s->cal_stop = false;
                                s->running  = false;
                            }
                        }
                    }

                    /* Report the reconstruction whenever k is known.
                     *
                     * ⚠️ Only when the three channels it is built from are on
                     * scale. A pinned channel says "at least full scale", so
                     * summing three of them and scaling gives a number that
                     * cannot exist: seen on HW as VIS=155279 where the
                     * saturation ceiling for that channel is 71678 - more than
                     * twice the maximum, printed as if it were a measurement.
                     * With R, G or B pinned the reconstruction is reported as
                     * "--", which is what an unavailable value looks like
                     * everywhere else in the row. */
                    bool rgb_ok = (sample.saturated & 0x0Du) == 0u;  /* R,B,G */

                    if (s->cal_done && rgb_ok) {
                        /* rgb is already corrected and k was fitted on corrected
                         * counts, so the reconstruction lands directly in the
                         * same space as the measured channels: NO otp_apply()
                         * here. Applying the VIS coefficient on top would
                         * correct twice — it used to be needed only because the
                         * sum was taken on raw counts. */
                        sample.vis_calc = (u32)(((u64)rgb * s->cal_k) / CAL_K_SCALE);
                        sample.cal_k    = s->cal_k;
                    } else {
                        sample.vis_calc = 0;
                        /* Keep cal_k when it is merely this SAMPLE that cannot
                         * be reconstructed: the K column still describes the
                         * calibration in force, and only VIS/NVIS go to "--". */
                        sample.cal_k = (s->cal_done && !rgb_ok) ? s->cal_k : 0u;
                    }
                }

                /* ── Mixed acquisition: the corrected reading ────────────────
                 *
                 * VIS is MEASURED here - channel 2 is on the visible diodes for
                 * every sample of this mode - so the reconstruction above is
                 * computed but not used by the row this mode prints. What is
                 * used is the raw visible count minus the scaled dark, in that
                 * order and on that scale (see K_DARK_SCALE).
                 *
                 * The subtraction is on RAW counts of the SAME channel, so the
                 * per-part coefficient is a factor common to both terms: it is
                 * applied once, to the difference. Applying it to each term
                 * first would give the same number and cost two corrections.
                 * That is the OPPOSITE of the RGB sum above, which is
                 * multi-channel: there the coefficients differ per channel, so
                 * they do NOT factor out and each term must be corrected first
                 * (which is why that sum now runs on counts[], not raw[]).
                 *
                 * ⚠️ Note for whoever builds the threshold comparator: the
                 * reconstructed CVIS carries THREE channel darks (k*rgb, and rgb
                 * is a sum of three channels), so it sits about (3k-1)*d above a
                 * measured VIS. Harmless today, because CVIS is display-only and
                 * this mode always measures VIS. It stops being harmless the
                 * moment the comparator is fed the reconstruction instead: the
                 * alarm lives in darkness, which is exactly where d stops being
                 * negligible. */
                /* ── Photometry ──────────────────────────────────────────
                 * Computed HERE, in the task that owns the sensor, and carried
                 * in the sample like everything else: the printing task must
                 * never reach back into s_sensor[] for the settings a row was
                 * taken at.
                 *
                 * Unconditional, not only in the mixed acquisition. It costs a
                 * handful of 64-bit divides once per 400 ms frame, and having
                 * the number on every sample means the column can move to
                 * another table without moving the computation with it.
                 *
                 * ⚠️ counts[], not raw[]: the per-channel OTP coefficients
                 * differ, and a three-channel matrix product does not let them
                 * factor out the way a single-channel subtraction does. */
                {
                    lux_photo_t ph;
                    bool ok = lux_photometric(sample.counts, sample.gain,
                                              sample.exp_steps, &ph);
                    /* ⚠️ A pinned R, G or B is not a measurement, so neither is
                     * the lux built from it - and the matrix would happily
                     * return a plausible number from three clipped channels.
                     * This is the same rule the signal column follows, applied
                     * to the three channels photometry actually reads. */
                    const u8 rgb_mask = (u8)((1u << CH_RED) | (1u << CH_GREEN)
                                                            | (1u << CH_BLUE));
                    if (sample.saturated & rgb_mask) ok = false;

                    sample.lux_milli = ok ? ph.lux_milli : 0u;
                    sample.lux_ok    = ok ? 1u : 0u;
                }

                /* ── Measuring Kdark ("lux kdark cal") ───────────────────
                 *
                 * Both terms come from THIS frame: channel 2 is on the covered
                 * diodes and carries the dark, channel 6 carries clear. No
                 * switching, no held value - see KDARK_SAMPLES for why that is
                 * what makes it automatable at all.
                 *
                 * ⚠️ raw[], not counts[]. meas_correct() subtracts raw from
                 * raw, so the coefficient has to be a ratio of raws or it
                 * arrives pre-loaded with the two channels' OTP coefficients. */
                if (s->kd_req > 0u && s->dark_mode) {
                    const u8 pin = (u8)((1u << CH_CLEAR) | (1u << CH_VIS));
                    const u32 rc = sample.raw[CH_CLEAR];
                    const u32 rd = sample.raw[CH_VIS];

                    if ((sample.saturated & pin) != 0u || rd == 0u) {
                        /* ⚠️ Skipped WITHOUT decrementing, so the average is
                         * built from KDARK_SAMPLES valid samples - but bounded,
                         * unlike the calibration above: a dark pinned at zero
                         * never becomes usable on its own, and waiting for it
                         * would leave the sensor running for ever with nothing
                         * on the console to say why. */
                        s->kd_skip++;
                        if (s->kd_skip >= KDARK_SKIP_MAX) {
                            /* ⚠️ These two messages must not say "raise the
                             * gain" or "lower it": the command already set both
                             * to the maximum itself. At full sensitivity a dark
                             * of zero is not a settings problem and a pinned
                             * channel is not a light-level problem - they each
                             * mean one specific thing, and naming it is the
                             * whole value of failing here. */
                            ca_stdout_printf_n(
                                "# sensor %s: Kdark ABANDONED after %u unusable"
                                " samples - %s", s->label, s->kd_skip,
                                (rd == 0u)
                                  ? "the dark reads 0 at FULL gain: channel 2 is"
                                    " not on the covered diodes, or the part is"
                                    " not converting"
                                  : "a channel is pinned at FULL gain: the"
                                    " sensors are NOT in the dark");
                            s->kd_req = 0u;
                            KDARK_RESTORE(s);
                        }
                    } else {
                        u32 r = (u32)(((u64)rc * K_DARK_SCALE) / rd);
                        if (s->kd_n == 0u) { s->kd_min = r; s->kd_max = r; }
                        else {
                            if (r < s->kd_min) s->kd_min = r;
                            if (r > s->kd_max) s->kd_max = r;
                        }
                        /* The ratio of the SUMS, not the mean of the ratios:
                         * it weights each sample by its own signal instead of
                         * letting the dimmest frame count as much as the
                         * brightest, which at these levels is where the noise
                         * lives. kd_min/kd_max keep the per-sample spread so
                         * the operator still sees the scatter the mean hides. */
                        s->kd_clear += (u64)rc;
                        s->kd_dark  += (u64)rd;
                        s->kd_n++;
                        s->kd_req--;

                        if (s->kd_req == 0u) {
                            u32 kd = (u32)((s->kd_clear * K_DARK_SCALE)
                                           / s->kd_dark);
                            u32 spread = kd ? (u32)(((u64)(s->kd_max - s->kd_min)
                                                     * 1000u) / kd) : 0u;

                            if (kd > KDARK_RATIO_MAX) {
                                /* ⚠️ Refused, not stored with a warning. A
                                 * ratio this high is light on CLEAR, and the
                                 * number would describe the bench lighting
                                 * rather than the part - then sit in the
                                 * config looking like a calibration. */
                                ca_stdout_printf_n(
                                    "# sensor %s: Kdark=%u.%03u REFUSED - above"
                                    " %u.%03u this is light, not darkness."
                                    " Value NOT changed (still %u.%03u)",
                                    s->label, kd / K_DARK_SCALE,
                                    kd % K_DARK_SCALE,
                                    KDARK_RATIO_MAX / K_DARK_SCALE,
                                    KDARK_RATIO_MAX % K_DARK_SCALE,
                                    s->k_dark / K_DARK_SCALE,
                                    s->k_dark % K_DARK_SCALE);
                            } else {
                                s->k_dark = kd;
                                ca_stdout_printf_n(
                                    "# sensor %s: Kdark=%u.%03u  (spread %u.%u%%,"
                                    " %u samples%s)  CLEAR/DARK on raw counts",
                                    s->label, kd / K_DARK_SCALE,
                                    kd % K_DARK_SCALE,
                                    spread / 10u, spread % 10u, s->kd_n,
                                    s->kd_skip ? ", some skipped" : "");
                                if (spread > 100u)
                                    ca_stdout_printf_n(
                                        "# sensor %s: spread above 10%% - the"
                                        " dark was not steady, treat this as"
                                        " indicative", s->label);
                            }

                            KDARK_RESTORE(s);
                        }
                    }
                }

                sample.meas       = s->meas_on ? 1u : 0u;
                sample.meas_ch    = s->meas_ch;
                sample.k_dark     = s->k_dark;
                sample.dark_used  = s->dark_hold;
                sample.dark_valid = (s->meas_on && s->dark_hold_ok) ? 1u : 0u;
                sample.dark_fresh = s->dark_was_fresh ? 1u : 0u;
                sample.corr_ok    = 0;
                sample.miscal     = 0;
                sample.sig_corr   = 0;
                sample.nsig_corr  = 0;
                s->dark_was_fresh = false;

                if (s->meas_on) {
                    s->vis_since_dark++;

                    /* ⚠️ A count of EXACTLY ZERO is NOT rejected here, and that
                     * is deliberate. It was, briefly, on the theory that an
                     * unrefreshed frame reads 0 - true, but so does a genuinely
                     * dark channel. Measured on HW at 1.00x/400 ms with the
                     * sensors under 3 mm caps AND a cover: ten consecutive rows,
                     * both parts, NDARK=0 with NVIS at its own floor of ~610.
                     * The dark floor at unity gain is BELOW one count, which is
                     * exactly what Table 16 implies by specifying the dark count
                     * noise at 66x and not at 1x.
                     *
                     * ⚠️ That covering was NOT verified light-tight - the bench
                     * has no such thing - but this conclusion does not need it
                     * to be. Residual light would RAISE these channels, and
                     * they read exactly 0; an imperfect cover therefore makes
                     * the statement stronger, not weaker. Conclusions that need
                     * real darkness, such as anything about how the CLEAR floor
                     * drifts, do NOT follow from this bench.
                     *
                     * So zero is a reading, and filtering it would blank the
                     * column precisely in darkness - the regime a threshold
                     * alarm lives in. The unrefreshed frames that motivated the
                     * filter only ever appeared with meas_discard at 0, which is
                     * a configuration the measurement has already ruled out; the
                     * protection against them is the discard, not a guess made
                     * from the value. */
                    /* The corrected channel is VIS or CLEAR depending on the
                     * mode; everything below is written against the index so
                     * the two share one path. The mask bit is the same index:
                     * saturated is a per-channel bitmask. */
                    const u8 mc = (s->meas_ch < ALS_CHANNELS) ? s->meas_ch
                                                              : (u8)MEAS_CH_VIS;

                    if (s->dark_hold_ok &&
                        (sample.saturated & (u8)(1u << mc)) == 0u) {
                        /* ⚠️ A pinned signal channel is not a measurement, so
                         * neither is anything derived from it: subtracting a
                         * dark from "at least full scale" yields a number that
                         * looks ordinary and is not one. */
                        bool mc_bad = false;
                        i32  corr = meas_correct(sample.raw[mc], s->dark_hold,
                                                 s->k_dark, &mc_bad);
                        sample.sig_corr = corr;
                        sample.miscal   = mc_bad ? 1u : 0u;
                        sample.corr_ok  = 1u;

                        /* ⚠️ The OTP coefficient of the SIGNAL channel, not of
                         * channel 2. In VIS mode the two are the same one and
                         * the factor genuinely cancels out of the difference;
                         * in CLEAR mode it does not, and applying channel 2's
                         * coefficient to a clear reading would correct it with
                         * another channel's calibration. What remains
                         * uncancelled - the dark's own coefficient - is
                         * absorbed by Kdark, which is measured in this mode
                         * anyway (see MEAS_CH_CLEAR). */
                        /* ⚠️ The OTP factor and the normalisation both take
                         * u32, and both are positive scalings - so the sign is
                         * carried around them rather than through them. Passing
                         * a negative residue in as u32 would scale 4e9 and come
                         * back as a huge positive: the exact wrap the signed
                         * meas_correct() was written to avoid, reintroduced two
                         * lines later. */
                        u32 mag = (corr < 0) ? (u32)(-(i64)corr) : (u32)corr;
                        if (sample.otp_on)
                            mag = otp_apply(mag,
                                       otp_calfactor(s->otp.gains[mc], sample.gain));
                        mag = norm_count(mag, sample.gain, sample.exp_steps);
                        sample.nsig_corr = (corr < 0) ? -(i32)mag : (i32)mag;
                    }
                }
                s->samples++;

                /* A frame identical to the one before means the part is not
                 * refreshing its registers - in mode 3 that is a missed
                 * acknowledgment (§3.3). Counting it turns "the numbers never
                 * change" from something you notice by eye into a number. */
                if (sample.counts[5] == s->last_clear) s->frozen++;
                else                                   s->frozen = 0;
                s->last_clear = sample.counts[5];

                /* Channel 2 carries whichever photodiodes the mode selects.
                 * In dark mode its count IS the dark reference, so it is
                 * reported as such instead of as "vis" - no second read, no
                 * switching inside the sample. That switching is what made the
                 * old dark track the light; see vd_set_dark_mode().
                 *
                 * NOT subtracted from anything: every channel already has its
                 * dark level removed in the analog chain (DC_CLAMP_EN for
                 * CH1..CH5, AC_CLAMP_EN for CH6). It is a check on that zero,
                 * not a correction to apply. */
                sample.dark_mode = s->dark_mode ? 1u : 0u;
                sample.dark      = s->dark_mode ? sample.counts[1] : 0u;
                if (s->dark_mode) s->last_dark = sample.dark;

                /* Queue the complete sample - all 7 values, read back to back
                 * within the same sample window. Do not block on a full
                 * queue: the sensor keeps producing whether the printer keeps
                 * up or not, and a measurement task stalled on a queue would
                 * silently stop reading the part. Count the loss instead —
                 * "show" reports it. */
                /* Autogain, before the sample is queued so the line can say
                 * that the parameters moved underneath it. The decision uses
                 * the raw counts: it is about ADC headroom, which the per-part
                 * correction does not change. */
                /* ⚠️ Held off while k is being measured. The autogain reacts
                 * to the light and can change gain mid-calibration: on HW that
                 * left sensor A with its first sample at 1.00x and the other
                 * fifteen at 2.50x, dragging the averaged k by 0.66% and
                 * showing up as a 0.8% spread where the other sensor - whose
                 * gain never moved - reported 0.0%.
                 *
                 * k is a ratio between channels, so it should not depend on
                 * the gain at all; that it does, slightly, is the residue of
                 * the per-channel corrections not being perfectly linear. The
                 * fix is simply to calibrate at fixed settings, which is what
                 * calibration means. Sixteen samples is under seven seconds -
                 * the autogain has nothing to do that cannot wait. */
                sample.adjusted = 0;
                /* ⚠️ This sample was taken while a change made after the
                 * PREVIOUS one was still working through, so its frame
                 * straddles two settings. It has to be excluded from any fit.
                 *
                 * It used to carry no mark at all: ADJ=1 sits on the row
                 * BEFORE the change, so the contaminated row - the one after -
                 * looked ordinary. On HW that put NDARK=878 between neighbours
                 * reading 647-664, and NDARK=4644 between neighbours reading
                 * 601-677, with nothing in the row to say why. The comment
                 * below claimed the sample was binned; `settle` only stopped
                 * the autogain from reacting again, and the sample was queued
                 * and printed like any other. Marking it is enough - dropping
                 * it would leave a gap in SEQ that looks like a lost sample. */
                sample.settling = (s->settle > 0u) ? 1u : 0u;
                if (s->auto_gain && s->cal_req == 0u &&
                    s->settle == 0u && ag_update(s, &sample)) {
                    if (ag_apply(s)) {
                        sample.adjusted = 1u;
                        s->adjusts++;
                        /* The frame in flight is integrating with the old
                         * settings: the NEXT sample straddles the change and
                         * is marked "S" in the ADJ column. */
                        s->settle = AG_SETTLE;
                        /* ⚠️ The held dark was taken at the settings that have
                         * just been left behind, and the mixed acquisition
                         * subtracts it from raw counts BEFORE normalising -
                         * i.e. it assumes both terms share a gain. Re-measure
                         * it before the next sample rather than carrying a
                         * value from the old scale across the step. */
                        s->dark_forced = true;
                    } else {
                        s->errors++;
                    }
                } else if (s->settle > 0u) {
                    s->settle--;
                }

                if (!ca_queue_static_trysend(&s_queue.q, &sample)) s->dropped++;

                EVAL_LED( ca_led_toggle((u8)(idx + 1u)); )
            } else {
                s->errors++;
            }

            /* A sweep requested from the CLI runs HERE, for the same reason
             * as the mode change below: this task owns the bus. It takes
             * PROBE_COUNT x 2 frames, during which no samples are produced -
             * that is expected, and the sweep says so when it starts. */
            if (s->probe_req) {
                u8 ch = (u8)(s->probe_req - 1u);
                s->probe_req = 0;
                vd_probe_sweep(s, ch);
                if (s->probe_stop) {        /* the sweep started it: park it */
                    s->probe_stop = false;
                    s->running    = false;
                }
            }

            /* Same reason, same place: the byte dump reads the channel group
             * directly and needs the bus this task owns. */
            if (s->bytes_req) {
                s->bytes_req = false;
                vd_dump_bytes(s);
                if (s->bytes_stop) {
                    s->bytes_stop = false;
                    s->running    = false;
                }
            }

            /* A mode change requested from the CLI is applied HERE, by the
             * task that owns the bus - never from the CLI thread, which would
             * write SEL_PD in the middle of a transaction this task is running.
             * The frame in flight straddles the switch, so it is discarded. */
            if (s->dark_req != s->dark_mode) {
                if (vd_set_dark_mode(s, s->dark_req)) {
                    /* ⚠️ Say so IN the capture, and reprint the column names.
                     *
                     * Columns 4 and 9 (and their normalised twins) are named by
                     * the mode in force when the header was emitted, and that
                     * header goes out once, at
                     * start. "lux cal" turns dark off for CAL_SAMPLES and back
                     * on afterwards, so a capture that begins with a
                     * calibration changes what column 9 HOLDS without changing
                     * what it is CALLED. Seen on HW: rows 1-16 carried a
                     * reconstructed 2088277 and row 17 onwards a measured dark
                     * of 1588, both under "CVIS" - two different quantities in
                     * one column, named after the wrong one.
                     *
                     * The "#" line is the authoritative record: it names the
                     * sensor, which the shared header cannot when the two
                     * switch on different samples. */
                    ca_stdout_printf_n("# sensor %s: channel 2 -> %s diodes,"
                                       " col 4 is now %s and col 9 %s", s->label,
                                       s->dark_mode ? "COVERED" : "visible",
                                       s->dark_mode ? "CVIS" : "VIS",
                                       s->dark_mode ? "DARK" : "CVIS");
                    print_header();
                    if (!vd_next_frame(s)) { s->errors++; continue; }  /* mixed */
                } else {
                    s->errors++;
                    s->dark_req = s->dark_mode;     /* failed: stop retrying */
                }
            }

            /* One clean frame before the next read. The wait is on INTR_ST -
             * the sensor says when a frame is done, we do not guess from a
             * delay. Only ONE window per sample now: with no switching to
             * undo, the old four-window cycle is down to one. */
            if (!vd_next_frame(s)) { s->errors++; continue; }
        }

        /* Last one out reopens the header for the next run: a capture that
         * follows a stop is a new capture and needs its own column names. */
        {
            bool any = false;
            for (u8 i = 0; i < SENSOR_COUNT; i++)
                if (s_sensor[i].running) any = true;
            if (!any) s_header_done = false;
        }

        /* "lux stop" cleared the flag. Disable the ALS engine here, in the
         * task that owns the bus, rather than from the CLI: that keeps every
         * I2C transaction for this sensor on one thread. */
        if (vd_write_reg(s->bus, VD6283_ALS_CTRL, 0x00u))
            ca_stdout_printf_n("lux: sensor %s stopped, ALS disabled", s->label);
        else
            ca_stdout_printf_n("lux: sensor %s stopped, but ALS_CTRL write failed",
                               s->label);
    }
    return 0;
}

/* ============================================================================
 * PRINT TASK — drains the queue
 * ========================================================================== */

/* ── Tabular output ─────────────────────────────────────────────────────────
 *
 * The row is assembled into ONE static buffer and printed with a single "%s".
 *
 * ⚠️ Not with a 16-argument printf. ca_stdout_printf_n formats into a
 * DEFAULT_MSG_SIZE (132) byte buffer ON ITS OWN STACK, and this task runs on
 * STACK_SMALL; adding a dozen local column buffers and a 16-argument va_list
 * on top of that overflowed it, and the column pointers ended up outside the
 * memory map. ca_valid_pointer() then rejected them and every field printed
 * "( err : invalid pointer )" - the check did its job and stopped a fault.
 *
 * One static buffer, one argument: nothing large on the stack, and the row can
 * be as wide as it needs to be. */

#define ROW_MAX     168u        /* row is ROW_WIDTH; the slack is for safety */

/* ── Column widths ──────────────────────────────────────────────────────────
 *
 * ⚠️ THE ROW HAS A HARD CEILING. ca_stdout_printf_n() formats into
 * DEFAULT_MSG_SIZE - sizeof(u16) = 130 bytes and appends CRLF only while the
 * text is SHORTER than 128 - so a row of 128 characters or more comes out with
 * no line ending, and the next line is pasted onto it. Past 129 ca_vsnprintf
 * also truncates and stamps "~OVF". Both were happening: the 24-bit widening
 * took the row to 136 and the capture became unreadable.
 *
 * Every field costs width + 1 (row_num appends one space). ROW_WIDTH below is
 * the exact total and the _Static_assert enforces it, so a column added later
 * fails the BUILD instead of quietly mangling every capture.
 *
 * COUNT_W is eight because a 24-bit count runs to 16777215, and the autogain
 * targets 70% of full scale - eight digits is the normal case, not the corner.
 * That is the fixed cost: nine count columns are 81 of the 127 characters. The
 * rest was trimmed to fit, and two per-capture fields left the row entirely
 * (see the "#" banner): the OTP-off note and K, which is frozen after
 * "lux cal" and identical on every row of a capture. */
#define COUNT_W     8u          /* the nine count columns */
#define W_SENS      2u          /* "A"/"B", left-aligned; header "S" */
#define W_SEQ       5u
#define W_DV        6u          /* parts per 100000, ceiling 100000 */
#define W_FS        3u          /* 0..100 */
#define W_SAT       3u
#define W_GAIN      5u          /* row_str writes "66.67" + a space */
#define W_EXP       4u          /* 3..1638 */
#define W_DROP      4u
#define W_ADJ       3u

/* The mixed-acquisition row (see K_DARK_SCALE) is a different table, not a
 * variant of the one below: it drops the nine per-channel counts and reports
 * two derived quantities. Roughly half the width - but it has its own assert
 * anyway, below. */
#define W_KDARK     6u          /* Kdark x1000, up to K_DARK_MAX = 100000 */
#define W_MARK      3u          /* the per-row marker column */
/* Lux x1000. Nine digits reach 999999 lx - past direct sunlight (~120000), so
 * the column cannot overflow on any light this part can see, and a value that
 * needed ten digits would mean the matrix had been fed nonsense. */
#define W_LUX       9u

/* This table used to be exempt from the width check, on the grounds that it is
 * far narrower than the one below. That reasoning was a sentence in a comment,
 * and a sentence does not fail a build - it stopped being true the moment a
 * column was added to the table, which is exactly when nobody re-reads it. So
 * it gets its own assert, for the same reason ROW_WIDTH has one. */
#define MEAS_ROW_WIDTH (W_SENS + (W_SEQ + 1u) + (W_LUX + 1u) + (COUNT_W + 1u) \
                       + (W_FS + 1u) + (COUNT_W + 1u) + (W_KDARK + 1u) \
                       + (W_GAIN + 1u) + (W_EXP + 1u) + (W_MARK + 1u))

_Static_assert(MEAS_ROW_WIDTH < DEFAULT_MSG_SIZE - sizeof(u16) - 2u,
               "mixed-acquisition row must fit the line or it loses its CRLF");
_Static_assert(MEAS_ROW_WIDTH < ROW_MAX, "mixed-acquisition row must fit s_row");

#define ROW_WIDTH   (W_SENS + (W_SEQ + 1u) + 9u * (COUNT_W + 1u) \
                    + (W_DV + 1u) + (W_FS + 1u) + (W_SAT + 1u) + (W_GAIN + 1u) \
                    + (W_EXP + 1u) + (W_DROP + 1u) + (W_ADJ + 1u))

_Static_assert(ROW_WIDTH < DEFAULT_MSG_SIZE - sizeof(u16) - 2u,
               "table row must fit ca_stdout_printf_n's line or it loses its CRLF");
_Static_assert(ROW_WIDTH < ROW_MAX, "table row must fit s_row");

static char s_row[ROW_MAX];     /* only the printing task writes this */

/* Append a right-aligned number - or "--" when the field does not apply - in a
 * fixed width, plus one trailing space. Returns the new offset.
 * ca_vsnprintf has no "%*u", so the padding is done here: without it the
 * columns drift as soon as a value changes digits, and a drifting column is
 * not importable. */
static u32 row_num(char *dst, u32 off, u32 width, u32 v, bool valid)
{
    char tmp[12];
    u32  n = 0;

    if (!valid) {
        tmp[n++] = '-'; tmp[n++] = '-';
    } else if (v == 0u) {
        tmp[n++] = '0';
    } else {
        char rev[12]; u32 r = 0;
        while (v > 0u && r < sizeof(rev)) { rev[r++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (r > 0u) tmp[n++] = rev[--r];
    }
    /* ⚠️ A value too wide for its column used to be cut to the first `width`
     * digits - keeping the most significant ones and dropping the rest, which
     * yields a plausible smaller number rather than an obvious fault. The
     * normalised columns can still reach it: norm_count() scales by
     * 256 * 400 / (gain * exp_ms), so a bright sample taken at minimum gain and
     * a short exposure runs past eight digits. Fill the field instead: '#' is
     * not a number, so a spreadsheet rejects the cell rather than plotting a
     * value that is off by a power of ten. */
    if (n > width) {
        for (u32 i = 0; i < width && off < ROW_MAX - 1u; i++) dst[off++] = '#';
        if (off < ROW_MAX - 1u) dst[off++] = ' ';
        dst[off] = '\0';
        return off;
    }

    u32 pad = (width > n) ? (width - n) : 0u;
    for (u32 i = 0; i < pad && off < ROW_MAX - 1u; i++) dst[off++] = ' ';
    for (u32 i = 0; i < n   && off < ROW_MAX - 1u; i++) dst[off++] = tmp[i];
    if (off < ROW_MAX - 1u) dst[off++] = ' ';
    dst[off] = '\0';
    return off;
}


/* Append a literal, bounded by the same buffer end. */
/* The signed twin of row_num(), for the one column that can legitimately go
 * below zero (see sig_corr). Not a flag on row_num: every other numeric column
 * in both tables is a count or a percentage and cannot be negative, and giving
 * them all a sign-capable path would invite one to acquire a "-" from a bug
 * instead of from a measurement.
 *
 * The sign eats one character of the field, which is why the caller must size
 * the column for it - COUNT_W is eight, so seven digits and a sign. */
static u32 row_snum(char *dst, u32 off, u32 width, i32 v, bool valid)
{
    if (!valid || v >= 0)
        return row_num(dst, off, width, (u32)(v < 0 ? 0 : v), valid);

    {
        char tmp[12];
        u32  n = 0;
        u32  m = (u32)(-(i64)v);
        char rev[12]; u32 r = 0;

        tmp[n++] = '-';
        if (m == 0u) tmp[n++] = '0';
        else {
            while (m > 0u && r < sizeof(rev)) { rev[r++] = (char)('0' + (m % 10u)); m /= 10u; }
            while (r > 0u) tmp[n++] = rev[--r];
        }
        if (n > width) {
            for (u32 i = 0; i < width && off < ROW_MAX - 1u; i++) dst[off++] = '#';
            if (off < ROW_MAX - 1u) dst[off++] = ' ';
            dst[off] = '\0';
            return off;
        }
        u32 pad = (width > n) ? (width - n) : 0u;
        for (u32 i = 0; i < pad && off < ROW_MAX - 1u; i++) dst[off++] = ' ';
        for (u32 i = 0; i < n   && off < ROW_MAX - 1u; i++) dst[off++] = tmp[i];
        if (off < ROW_MAX - 1u) dst[off++] = ' ';
        dst[off] = '\0';
        return off;
    }
}

static u32 row_str(char *dst, u32 off, const char *src)
{
    while (*src && off < ROW_MAX - 1u) dst[off++] = *src++;
    dst[off] = '\0';
    return off;
}

/*
 * The gain column: exactly W_GAIN characters plus a space ("_1.00" / "66.67").
 *
 * It is the one non-integer field, so row_num() cannot emit it - printed raw it
 * is 4 or 5 wide and would shift every column after it on exactly the dark and
 * bright rows worth importing.
 *
 * ⚠️ ROUNDED, not truncated. 0x42AB is 66.668x: truncating gave "66.66" in the
 * row while the menu, which goes through GAIN_MILLI, said "66.67" for the same
 * gain - two renderings of one number that disagreed. The +128 is half a step of
 * the 8.8 fraction; the guard covers a fraction that rounds up to a whole unit
 * (no entry in GAIN_VALUE does, but the table is data and may change).
 *
 * Shared by both tables. Two copies drifted apart the moment the second one was
 * written, which is how the truncation above survived in one of them.
 */
static u32 row_gain(char *dst, u32 off, u16 gain)
{
    u32 whole = gain >> 8;
    u32 frac  = (u32)((((gain & 0xFFu) * 100u) + 128u) >> 8);
    char g[8]; u32 n = 0;

    if (frac >= 100u) { frac = 0u; whole++; }

    if (whole >= 10u) g[n++] = (char)('0' + (whole / 10u) % 10u);
    else              g[n++] = ' ';
    g[n++] = (char)('0' + (whole % 10u));
    g[n++] = '.';
    g[n++] = (char)('0' + (frac / 10u) % 10u);
    g[n++] = (char)('0' + (frac % 10u));
    g[n++] = ' ';
    g[n]   = '\0';

    return row_str(dst, off, g);
}

/*
 * The column header. Printed when a sensor starts and on demand ("lux head"),
 * so a capture pasted into a spreadsheet carries its own column names.
 *
 * ⚠️ VIS and DARK are the SAME channel (channel 2), never both at once: the
 * inactive one reads "--". Two columns rather than one renamed field, because
 * a column that changes meaning halfway down a capture cannot be imported -
 * the spreadsheet would average visible light with a dark reference.
 */
static void print_header(void)
{
    /* The mixed acquisition prints its own, much narrower table: the per-channel
     * counts are not what that mode is for, and leaving them in would bury the
     * one number it exists to produce. Taken from sensor A - "lux meas" arms
     * both, so the two are never in different modes. */
    if (s_sensor[0].meas_on) {
        /* Same widths row_num()/row_gain() emit, spelled out so a name and its
         * column cannot drift apart: W_SEQ, COUNT_W, W_FS, COUNT_W, W_KDARK,
         * W_GAIN, W_EXP, W_MARK - each followed by one space.
         *
         * ⚠️ Column 3 is named for the CHANNEL the mode corrects, exactly like
         * the VIS/CVIS pair below: "lux meas_cl" puts CLEAR there, and a
         * capture whose header says NVIS while the numbers are clear counts is
         * unreadable six months later. Both names are COUNT_W wide, so the
         * table keeps its shape either way. */
        ca_stdout_printf_n(
            "S "  "  SEQ "  "     mLUX "  "%s "  "%%FS "  "   NDARK "  " KDARK "
            "GAINx "  " EXP "  "MRK",
            (s_sensor[0].meas_ch == MEAS_CH_CLEAR) ? "    NCLR" : "    NVIS");
        return;
    }

    /* FOUR names move with the mode, in two pairs, because channel 2 can only
     * hold one of its two roles at a time (see the row builder):
     *
     *   dark off -> VIS  = measured,      CVIS  = reconstructed
     *   dark on  -> CVIS = reconstructed, DARK  = measured
     *
     * and the normalised pair follows the same swap (NVIS/NCVIS, NCVIS/NDARK).
     * Naming column 4 "VIS" in dark mode was a lie of the same family as the
     * one column 9 already avoided: the column held the reconstruction while
     * the header claimed a measurement, and a capture read months later has
     * only the header to go by.
     *
     * The field COUNT never changes either way, so a capture that spans the
     * switch still imports - only the names say which quantity is where. */
    bool dk = s_sensor[0].dark_mode;

    /* Built to the SAME widths row_num() uses, so every name sits over its
     * column. Written by hand it was off by two from GAINx onwards - invisible
     * on screen, but a fixed-width import lines up on the header.
     *
     * Each field below is "name right-aligned in W" followed by one space,
     * which is exactly what row_num() emits, and the padding is spelled out so
     * a name and its column cannot drift apart. The order and the widths are
     * the W_* macros; change one and its name here changes with it. */
    ca_stdout_printf_n(
        "S "  "  SEQ "  "     RED "  "%s "  "   GREEN "  "    BLUE "
        "      IR "  "   CLEAR "  "%s "  "   D/V "  "%s "  "%s "
        "%%FS "  "SAT "  "GAINx "  " EXP "  "DROP "  "ADJ",
        dk ? "    CVIS" : "     VIS",   /* col 4  - channel 2 or the rebuild */
        dk ? "    DARK" : "    CVIS",   /* col 9  - the other one */
        dk ? "   NCVIS" : "    NVIS",   /* col 11 - normalised col 4 */
        dk ? "   NDARK" : "   NCVIS");  /* col 12 - normalised col 9 */
}

/*
 * The mixed-acquisition row: normalised corrected VIS, its headroom, the dark
 * it was corrected with, and the coefficient.
 *
 * ⚠️ Everything printed here comes out of the SAMPLE. Reading s->meas_on or
 * s->k_dark from s_sensor[] instead would race the measurement task, which owns
 * that table and moves it - the autogain can change the gain between the moment
 * a sample is queued and the moment this task gets to it, and the row would
 * then describe a setting the reading was not taken at.
 */
static void print_sample_meas(const lux_sample_t *s)
{
    const char *who = (s->sensor < SENSOR_COUNT) ? s_sensor[s->sensor].label : "?";
    u32 off = 0;

    /* Range-checked for the same reason the sensor index above is: the row
     * arrives through a queue, and "1u << meas_ch" on a byte that came back
     * wrong is undefined behaviour rather than a wrong character. */
    const u8 mc = (s->meas_ch < ALS_CHANNELS) ? s->meas_ch : (u8)MEAS_CH_VIS;

    /* ── EVERY quantity on this row is on the NORMALISED scale ──────────────
     *
     * ⚠️ %FS is a percentage of NORM_FULL_SCALE, not of the raw ceiling in
     * force. Measured on HW with the raw ceiling: two consecutive rows whose
     * NVIS differed by 1% reported 89% and 24%, because the autogain had
     * stepped between them. The percentage was describing the settings, not the
     * light. Against the unified scale both rows read 12%.
     *
     * ⚠️ NDARK is normalised for the same reason, and it is the more important
     * of the two: the dark is what this mode exists to measure. On HW one
     * sensor's held dark read 97111, 32144, 3367 and 2361 over four autogain
     * steps - a factor of 41 across readings of ONE unchanging dark current. A
     * column that does that cannot be plotted against temperature, which is the
     * entire purpose.
     *
     * How close the ADC actually is to clipping is a different question, and it
     * is answered by the "X" marker below rather than by a percentage. */
    /* ⚠️ %FS is taken from the residue clamped at zero, not from the residue.
     * A negative signal has no headroom to express as a percentage of full
     * scale - it is BELOW the floor, and 0% is the true answer. Feeding the
     * signed value to an unsigned percentage would have printed 4e9 % once the
     * residue went negative, which is now the normal state in darkness. */
    u32 nsig  = (s->nsig_corr < 0) ? 0u : (u32)s->nsig_corr;
    u32 fs    = (u32)(((u64)nsig * 100u) / NORM_FULL_SCALE);
    u32 ndark = norm_count(s->dark_used, s->gain, s->exp_steps);
    if (fs > 100u) fs = 100u;

    off = row_str(s_row, off, who);
    while (off < W_SENS && off < ROW_MAX - 1u) { s_row[off++] = ' '; s_row[off] = '\0'; }

    off = row_num(s_row, off, W_SEQ,   s->seq,       true);
    /* ⚠️ Gated on lux_ok, NOT on corr_ok: the two answer different questions
     * and can disagree in both directions. Sharing one flag would have blanked
     * lux whenever the signal channel clipped, and printed one whenever R, G
     * or B did. */
    off = row_num(s_row, off, W_LUX,   s->lux_milli, s->lux_ok != 0u);
    off = row_snum(s_row, off, COUNT_W, s->nsig_corr, s->corr_ok != 0u);
    off = row_num(s_row, off, W_FS,    fs,           s->corr_ok != 0u);
    off = row_num(s_row, off, COUNT_W, ndark,        s->dark_valid != 0u);
    off = row_num(s_row, off, W_KDARK, s->k_dark,    true);

    /* ⚠️ GAIN and EXP are not decoration. Everything to their left is
     * normalised, so it no longer says what the reading was actually taken at -
     * and without that a capture cannot be checked, only believed. The first HW
     * run of this mode could not be diagnosed at all because these two columns
     * were missing: there was no way to tell a dark that had moved from a gain
     * that had moved under it. */
    off = row_gain(s_row, off, s->gain);
    off = row_num(s_row, off, W_EXP, (u32)s->exp_ms, true);

    /* One marker column, most serious first. A row can be several of these at
     * once and only one character is available, so the order is the order in
     * which they invalidate the reading:
     *
     *   "!"  Kdark is out of calibration for this part: the residue sits more
     *        than MEAS_MISCAL_PCT below zero relative to what was subtracted.
     *        ⚠️ NOT "the residue is negative". At the floor half the rows are,
     *        by construction, and a marker on half the capture marks nothing -
     *        which is what it did before "lux kdark cal" existed to measure the
     *        coefficient instead of guessing it;
     *   "X"  the SIGNAL channel is pinned, so nothing derived from it is a
     *        measurement and the count/%FS pair read "--". Without this marker
     *        those dashes have no stated cause, and saturation looks like a
     *        failed transaction;
     *   "S"  the frame straddles a settings change (see AG_SETTLE);
     *   "D"  the dark was re-measured immediately before this row - the only
     *        row in each block whose dark is contemporary with its VIS, and
     *        therefore the one a drift analysis should prefer;
     *   "--" an ordinary row, reusing a held dark. */
    if      (s->miscal)               off = row_str(s_row, off, "  ! ");
    else if (s->saturated & (u8)(1u << mc))
                                      off = row_str(s_row, off, "  X ");
    else if (s->settling)             off = row_str(s_row, off, "  S ");
    else if (s->dark_fresh)           off = row_str(s_row, off, "  D ");
    else                              off = row_num(s_row, off, W_MARK, 0u, false);

    ca_stdout_printf_n("%s", s_row);
}

static void print_sample(const lux_sample_t *s)
{
    /* The index comes back through the queue, so treat it as data rather than
     * trusting it: one bad byte here would index outside s_sensor[] and turn
     * a print into a fault. */
    const char *who = (s->sensor < SENSOR_COUNT) ? s_sensor[s->sensor].label : "?";

    if (s->meas) { print_sample_meas(s); return; }

    /* Headroom of the channel closest to clipping, against the SATURATION
     * level for this exposure. Below 0x048 steps that ceiling is well under the
     * 24-bit range, and taking the range instead reported single-digit "%FS"
     * while channels were already pinned.
     *
     * ⚠️ This is the MAXIMUM over all six channels, which is what an operator
     * wants from the column - how close the closest channel is to clipping.
     * ag_peak_pct() deliberately uses CLEAR alone, because clear is the channel
     * the autogain steers. The two numbers are not the same and are not meant
     * to be: a row can read 96 %FS on IR while the autogain sees clear at 60
     * and leaves the settings alone. */
    u32 peak = 0;
    for (u8 ch = 0; ch < ALS_CHANNELS; ch++)
        if (s->raw[ch] > peak) peak = s->raw[ch];
    u32 sat_lvl  = s->sat ? s->sat : ALS_SAT_LONG_EXP;
    u32 peak_pct = (u32)(((u64)peak * 100u) / sat_lvl);
    if (peak_pct > 100u) peak_pct = 100u;

    /* The gain column is built by row_gain() from the value carried by the
     * SAMPLE, not the sensor's current setting: the autogain may have moved on
     * since, and the row must describe the reading it belongs to. */

    /* Channel 2 feeds VIS or DARK, never both - see print_header(). */
    bool dk  = (s->dark_mode != 0u);
    u32  off = 0;

    /* Sensor label, left-aligned in W_SENS (the header's "S "). */
    off = row_str(s_row, off, who);
    while (off < W_SENS && off < ROW_MAX - 1u) { s_row[off++] = ' '; s_row[off] = '\0'; }

    off = row_num(s_row, off, W_SEQ,    s->seq,       true);
    off = row_num(s_row, off, COUNT_W,  s->counts[0], true);
    /* VIS: the MEASURED value whenever channel 2 holds it, the reconstruction
     * only when it does not (dark mode). Preferring the reconstruction while a
     * real one is available would put the same number in VIS and CVIS and
     * destroy the comparison that makes the reconstruction checkable. So:
     *
     *   dark off -> VIS = measured, CVIS = reconstructed   (check the method)
     *   dark on  -> VIS = reconstructed, DARK = measured   (do the experiment)
     */
    if (!dk)                    off = row_num(s_row, off, COUNT_W, s->counts[1], true);
    else if (s->vis_calc != 0u) off = row_num(s_row, off, COUNT_W, s->vis_calc,  true);
    else                        off = row_num(s_row, off, COUNT_W, 0u,           false);
    off = row_num(s_row, off, COUNT_W, s->counts[3], true);
    off = row_num(s_row, off, COUNT_W, s->counts[2], true);
    off = row_num(s_row, off, COUNT_W, s->counts[4], true);
    off = row_num(s_row, off, COUNT_W, s->counts[5], true);
    /* DARK when channel 2 holds the covered diodes; otherwise the RECONSTRUCTED
     * vis, which is what makes the reconstruction checkable against the
     * measured VIS sitting a few columns to the left. */
    if (dk) off = row_num(s_row, off, COUNT_W, s->counts[1], true);
    else    off = row_num(s_row, off, COUNT_W, s->vis_calc,  s->cal_k != 0u);

    /* D/V in PARTS PER 100000: the dark-to-visible ratio, which is what the
     * thermal correction is judged on.
     *
     * ⚠️ It was per mille, and that had no resolution left where it matters.
     * Measured on HW: 0.29 per mille in ordinary light, truncated to a printed
     * "0" - and bright light is exactly the regime an alarm threshold sits in,
     * because the dark is then a small fraction of the reading. Per 100000 the
     * same sample reads 29. The ceiling, dark equal to vis, is 100000. */
    {
        /* In dark mode the only vis available is the reconstruction - which is
         * the whole point: dark and vis then refer to the SAME instant. */
        u32 vis = dk ? s->vis_calc : s->counts[1];   /* 0 => "--" below */
        u32 dv  = (dk && vis > 0u)
                ? (u32)(((u64)s->counts[1] * 100000u) / vis) : 0u;
        off = row_num(s_row, off, W_DV, dv, (dk && vis > 0u));
    }

    /* ── Normalised columns ─────────────────────────────────────────────────
     * The same two quantities scaled to 1.00x and the default exposure, so
     * rows taken at different autogain settings - and rows from the two
     * sensors - can be compared and plotted directly. Without these, a gain
     * step makes the trace jump for a reason that has nothing to do with the
     * light. */
    {
        /* In dark mode NVIS rests on the reconstruction, which is 0 when the
         * channels it needs were pinned - then it prints "--" like VIS. */
        u32 vis_raw = dk ? s->vis_calc : s->counts[1];
        off = row_num(s_row, off, COUNT_W,
                      norm_count(vis_raw, s->gain, s->exp_steps), vis_raw != 0u);

        u32 ch2 = dk ? s->counts[1] : s->vis_calc;
        bool ch2ok = dk ? true : (s->cal_k != 0u);
        off = row_num(s_row, off, COUNT_W,
                      norm_count(ch2, s->gain, s->exp_steps), ch2ok && ch2 != 0u);
    }

    /* K is NOT a column. It is frozen by "lux cal" and identical on every row
     * of a capture, so it belongs to the "#" banner - which is where it is now
     * printed, per sensor. It used to sit here, and its six characters are part
     * of what took the row past the line limit once the counts needed eight
     * digits each. Same reasoning retired the trailing " OTP-OFF" note. */

    off = row_num(s_row, off, W_FS,  peak_pct, true);
    /* SAT as a COLUMN: 1 when any channel is pinned, "--" otherwise. A flag
     * appended after the last field is invisible to a fixed-width import, and
     * the saturated rows are exactly the ones a spreadsheet must filter out. */
    off = row_num(s_row, off, W_SAT, 1u, s->saturated != 0u);

    /* ⚠️ row_gain() emits exactly W_GAIN characters plus a space - the assert on
     * ROW_WIDTH counts the macro, so the two have to be changed together. */
    off = row_gain(s_row, off, s->gain);

    off = row_num(s_row, off, W_EXP, (u32)s->exp_ms, true);
    /* Samples the queue could not take. Normally 0; a rising DROP means the
     * printing is not keeping up with the measuring, and the capture has gaps
     * that SEQ alone would not reveal. */
    off = row_num(s_row, off, W_DROP, s->dropped,   true);

    /* ADJ as a COLUMN, like SAT: 1 when the autogain moved gain or exposure
     * right after this sample, "--" otherwise. Those rows are the ones whose
     * settings differ from the next, so a spreadsheet must be able to find
     * them - a note past the last field cannot be filtered on.
     *
     * "CAL" instead while k is being measured: those rows are real readings,
     * but they belong to the calibration and not to the run, so they must be
     * as easy to exclude as an autogain step. */
    /* "S" wins over "1": a row that both straddles a change and triggers a new
     * one is unusable either way, and "excluded from the fit" is the fact the
     * reader must not miss. */
    if (s->calibrating)     off = row_str(s_row, off, "CAL ");
    else if (s->settling)   off = row_str(s_row, off, "  S ");
    else                    off = row_num(s_row, off, W_ADJ, 1u, s->adjusted != 0u);

    ca_stdout_printf_n("%s", s_row);
}

/*
 * Wiring map, printed once at start-up.
 *
 * The SATEL-VD6283TX breakout brings the sensor out on a 4-pin connector
 * (doc/reference_manuals/vd6283tx_satel-2400566.pdf):
 *
 *     pin 1 = GND    pin 2 = 3V3    pin 3 = SCL    pin 4 = SDA
 *
 * Everything on the right-hand side below is a Zio connector position on this
 * board, so the map can be followed wire by wire without opening a manual.
 */
static void print_wiring_map(void)
{
    ca_stdout_printf_n("");
    ca_stdout_printf_n("=== app_50 lux sensor wiring map ==============================");
    ca_stdout_printf_n("SATEL-VD6283TX breakout: 1=GND  2=3V3  3=SCL  4=SDA");
    ca_stdout_printf_n("Both sensors keep I2C address 0x%02X, hence one bus each.",
                       VD6283_ADDR);
    ca_stdout_printf_n("");
    ca_stdout_printf_n("  SENSOR A            board");
    ca_stdout_printf_n("    1 GND    ------>  %s", PIN_GND_A);
    ca_stdout_printf_n("    2 3V3    ------>  %s", PIN_3V3_A);
    ca_stdout_printf_n("    3 SCL    ------>  %s", WIRE_A_SCL);
    ca_stdout_printf_n("    4 SDA    ------>  %s", WIRE_A_SDA);
    ca_stdout_printf_n("");
    ca_stdout_printf_n("  SENSOR B            board");
    ca_stdout_printf_n("    1 GND    ------>  %s", PIN_GND_B);
    ca_stdout_printf_n("    2 3V3    ------>  %s", PIN_3V3_B);
    ca_stdout_printf_n("    3 SCL    ------>  %s", WIRE_B_SCL);
    ca_stdout_printf_n("    4 SDA    ------>  %s", WIRE_B_SDA);
    ca_stdout_printf_n("");
    ca_stdout_printf_n("  Add a 4.7k pull-up to 3V3 on each SCL and SDA line");
    ca_stdout_printf_n("  unless the breakout already carries its own.");
    ca_stdout_printf_n("");
    ca_stdout_printf_n("  Both measurement tasks are IDLE:");
    ca_stdout_printf_n("    lux start       both sensors");
    ca_stdout_printf_n("    lux start 1     sensor A only (I2C%u)", s_sensor[0].bus);
    ca_stdout_printf_n("    lux start 2     sensor B only (I2C%u)", s_sensor[1].bus);
    ca_stdout_printf_n("  Bring them up one at a time when a bus is suspect:");
    ca_stdout_printf_n("  with both running there is no telling which one failed.");
    ca_stdout_printf_n("=========================================================");
    ca_stdout_printf_n("");
}

static u32 task_print(void *arg)
{
    (void)arg;
    lux_sample_t sample;

    /* Print the map before the measurement tasks report: they wait 200/500 ms
     * plus a full exposure, so the map lands first and stays readable. */
    ca_os_sleep(300);
    print_wiring_map();
    {   /* Report the gain actually configured, not a fixed "1x": the default
         * is a choice that can change, and a banner that lies about it is
         * worse than one that says nothing. */
        u32 gw, gf; gain_text(GAIN_IDX_DEFAULT, &gw, &gf);
        ca_stdout_printf_n("app_50: 2 x VD6283TX, exposure %u ms, gain %u.%02ux, 24-bit",
                           STEPS_TO_MS(EXTIME_STEPS), gw, gf);
    }

    while (1) {
        /* Wait forever: the task stays parked until a sender wakes it -
         * there is nothing else this task should be doing. */
        if (ca_queue_static_receive(&s_queue.q, &sample, CA_WAIT_FOREVER)) {
            print_sample(&sample);
            s_printed++;
        }
    }
    return 0;
}

/* ============================================================================
 * CLI
 * ========================================================================== */

#ifdef WIRE_A_PORT
/*
 * Continuity test. Drives each I2C pin as a plain push-pull output and reads
 * the other end back: it never touches the I2C peripheral, so a failure here
 * is wiring, not configuration. The measurement tasks must be idle — this
 * reprograms the very pins they use, so run it before wiring is trusted.
 */
static void wire_pin(u32 port, u32 pin)
{
    ca_gpio_init_port(port);
    ca_gpio_configure(port, pin, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);
}

static bool wire_test(u32 port, u32 pin, const char *txt)
{
    wire_pin(port, pin);

    ca_gpio_write(port, pin, 1);
    ca_os_sleep(2);
    bool hi = (ca_gpio_read(port, pin) != 0);

    ca_gpio_write(port, pin, 0);
    ca_os_sleep(2);
    bool lo = (ca_gpio_read(port, pin) == 0);

    ca_stdout_printf_n("  %s : %s", txt, (hi && lo) ? "toggles" : "STUCK");
    return hi && lo;
}
#endif

/* ============================================================================
 * SETUP MENU — interactive, numbered
 *
 * The caOS CLI has no "read a line and wait": a handler gets one line and
 * returns, and the next line is dispatched as a fresh command. So an
 * interactive menu has to be a STATE MACHINE - the handler remembers that a
 * menu is open and claims the next line as an answer instead of a command.
 *
 * The chain is walked before the built-in commands (cli_dispatch_chain in
 * caos/src/cli.c), so returning true here swallows the line, which is exactly
 * what an open menu needs: while it is up, "1" means "menu entry 1" and not
 * whatever else the system might make of it.
 *
 * ⚠️ The menu must be closable no matter what: an input state that cannot be
 * left would make the console useless until reboot. Every prompt takes an
 * empty line as "cancel", and "q" always quits from the top level.
 * ========================================================================== */

typedef enum {
    MENU_OFF = 0,       /* no menu: lines are ordinary commands */
    MENU_TOP,           /* showing the entries, waiting for a number */
    MENU_ASK_EXP,       /* waiting for an exposure in ms */
    MENU_ASK_GAIN,      /* waiting for a gain index */
    MENU_ASK_OTP,       /* waiting for on/off */
    MENU_ASK_AUTO,      /* waiting for on/off */
    MENU_ASK_DARK,      /* waiting for on/off */
} menu_state_t;

/* Which sensor the open menu is working on. The menu is opened by
 * "lux setup <n>", so the target is fixed for its whole life: no entry can
 * silently retarget it and change the wrong part. */
static menu_state_t s_menu   = MENU_OFF;
static u8           s_menu_s = 0;

/* ── Setup is COMMON to both sensors ────────────────────────────────────────
 *
 * This application exists to check that two sensors agree, so they must be
 * measuring under the same conditions: a gain or exposure set on one and not
 * the other turns every comparison into a comparison of CONFIGURATIONS, and
 * the difference looks like a sensor fault. So every manual change from the
 * menu is applied to both, and "lux setup 1" and "lux setup 2" show the same
 * settings by construction.
 *
 * ⚠️ The AUTOGAIN is deliberately NOT covered by this. It reacts to the light
 * each sensor actually receives, from inside that sensor's own task, and
 * forcing its correction onto the other one would push a sensor out of range
 * because its neighbour was lit differently. After an autogain step the two
 * can legitimately differ - the GAIN and EXP columns are in the output for
 * exactly that reason, so a capture always says what produced each row. */

/* Apply one manual setting to every sensor. The caller passes what to change;
 * only the fields it names are touched. */
typedef enum {
    SET_EXP = 0, SET_GAIN, SET_OTP, SET_AUTO, SET_DEFAULTS
} setup_field_t;

static void setup_all(setup_field_t what, u32 value)
{
    for (u8 i = 0; i < SENSOR_COUNT; i++) {
        sensor_ctx_t *t = &s_sensor[i];
        switch (what) {
        /* Gain and exposure invalidate a held dark: the mixed acquisition
         * subtracts it before normalising, so the two terms must share the
         * settings they were taken at. Same reason as the autogain path. */
        case SET_EXP:   t->exp_steps = (u16)value; t->dark_forced = true; break;
        case SET_GAIN:  t->gain_idx  = (u8)value;  t->dark_forced = true; break;
        case SET_OTP:   t->otp_on    = (value != 0u); break;
        case SET_AUTO:  t->auto_gain = (value != 0u); break;
        case SET_DEFAULTS:
            t->exp_steps = EXTIME_STEPS;
            t->gain_idx  = GAIN_IDX_DEFAULT;
            t->otp_on    = true;
            t->auto_gain = false;
            t->dark_forced = true;
            break;
        default: break;
        }
    }
}

/* Entry numbers. Kept as an enum so the prompts, the dispatch and the printed
 * list cannot drift apart when an entry is inserted. */
enum {
    MI_EXP = 1, MI_GAIN, MI_OTP, MI_AUTO, MI_DARK, MI_RESET, MI_SHOW, MI_QUIT
};

/* The command list, shared by "help" and by the menu's own help entry: two
 * copies would drift the moment a command is added. */
static void print_commands(void)
{
    CLI_PRINTF_N("lux       : start [1|2] | stop [1|2]  (no number = both sensors)");
    CLI_PRINTF_N("lux dark  : <on|off> channel 2 on the covered diodes (both)");
    CLI_PRINTF_N("lux probe : <1|2> [ch] sweep SEL_PD bits on one channel");
    CLI_PRINTF_N("lux bytes : <1|2> dump the raw 4 bytes of every channel group");
    CLI_PRINTF_N("lux head  : reprint the column header (tabular output)");
    CLI_PRINTF_N("lux cal   : measure k = VIS/(R+G+B)  -- needs a LIT, steady bench");
    CLI_PRINTF_N("lux meas  : <div> [discard] mixed VIS/DARK, NVIS = norm(VIS - Kdark*dark)");
    CLI_PRINTF_N("lux meas_cl: <div> [discard] the same on CLEAR (ch6, widest band)");
    CLI_PRINTF_N("lux kdark : <1|2> <val> set the per-sensor dark coefficient | cal = measure it");
    CLI_PRINTF_N("lux lx    : lux and colour temperature from R,G,B (one immediate read)");
    CLI_PRINTF_N("lux period: <ms> idle gap between measurements (0 = back-to-back)");
    CLI_PRINTF_N("lux clamp : <on|off> DC clamp on channel 2 (off = dark current survives)");
    CLI_PRINTF_N("lux otp   : <1|2> factory calibration read from the part");
    CLI_PRINTF_N("lux setup : interactive menu, applies to BOTH sensors");
    CLI_PRINTF_N("map       : sensor-to-connector wiring map");
    CLI_PRINTF_N("scan      : probe every I2C address on both buses");
    CLI_PRINTF_N("reg       : <addr> [val] raw register access, hex (sensor B)");
    CLI_PRINTF_N("show      : sensor status, configuration and counters");
    CLI_PRINTF_N("raw       : one immediate read from both sensors");
#ifdef WIRE_A_PORT
    CLI_PRINTF_N("wire      : I2C pin continuity test (sensors idle)");
#endif
}

static void menu_show(const sensor_ctx_t *s)
{
    u32 gw, gf;
    gain_text(s->gain_idx, &gw, &gf);

    CLI_PRINTF_N("");
    CLI_PRINTF_N("=== lux setup - BOTH sensors (A on I2C%u, B on I2C%u) ===",
                 s_sensor[0].bus, s_sensor[1].bus);
    CLI_PRINTF_N("every change below applies to both, so the two stay comparable");

    /* ⚠️ The values listed are ONE sensor's, and the two can genuinely differ:
     * the autogain moves gain and exposure from inside each measurement task,
     * independently, so turning it off freezes them wherever they happened to
     * be. Seen on HW - this menu said 358 ms while sensor B was running at 352.
     * A menu that says "applies to both" while showing only one of them is
     * lying by omission, so the other is named whenever it disagrees. */
    {
        const sensor_ctx_t *o = (s == &s_sensor[0]) ? &s_sensor[1] : &s_sensor[0];
        u32 ogw, ogf; gain_text(o->gain_idx, &ogw, &ogf);

        if (o->exp_steps != s->exp_steps)
            CLI_PRINTF_N(" %d) exposure ....... %u ms  (EXTIME=0x%03X, ~%u ms/sample)"
                         "   !! %s is at %u ms",
                         MI_EXP, exposure_ms(s), (u32)s->exp_steps,
                         SAMPLE_PERIOD_MS(s), o->label, exposure_ms(o));
        else
            CLI_PRINTF_N(" %d) exposure ....... %u ms  (EXTIME=0x%03X, ~%u ms/sample)",
                         MI_EXP, exposure_ms(s), (u32)s->exp_steps,
                         SAMPLE_PERIOD_MS(s));

        if (o->gain_idx != s->gain_idx)
            CLI_PRINTF_N(" %d) gain ........... %u.%02ux  (index %u)"
                         "   !! %s is at %u.%02ux",
                         MI_GAIN, gw, gf, (u32)s->gain_idx, o->label, ogw, ogf);
        else
            CLI_PRINTF_N(" %d) gain ........... %u.%02ux  (index %u)",
                         MI_GAIN, gw, gf, (u32)s->gain_idx);
    }
    CLI_PRINTF_N(" %d) otp correction . %s%s", MI_OTP, s->otp_on ? "on" : "off",
                 s->otp.read ? "" : "  (OTP never read - nominal)");
    CLI_PRINTF_N(" %d) autogain ....... %s", MI_AUTO, s->auto_gain ? "on" : "off");
    /* The parenthetical states what channel 2 is ACTUALLY on. It used to be the
     * fixed text "(channel 2 on the covered diodes)", printed even with the
     * mode off - so a capture recorded "dark mode off (channel 2 on the covered
     * diodes)", which says the opposite of the truth. */
    CLI_PRINTF_N(" %d) dark mode ...... %s  (channel 2 on the %s diodes)",
                 MI_DARK, s->dark_mode ? "on" : "off",
                 s->dark_mode ? "covered" : "visible");
    CLI_PRINTF_N(" %d) reset to defaults", MI_RESET);
    CLI_PRINTF_N(" %d) show otp details", MI_SHOW);
    CLI_PRINTF_N(" %d) quit", MI_QUIT);
    CLI_PRINTF_N("select 1-%d  (h = help, q = quit):", MI_QUIT);
}

/* Print the factory calibration. Shared by menu entry 6 and "lux otp". */
static void otp_report(sensor_ctx_t *s, int n)
{
    (void)n;

    /* The OTP is normally read by vd_configure() at start-up, but asking for
     * it before ever starting the sensor is the natural thing to do - it is a
     * property of the part, not of a measurement. So read it here when it is
     * missing, rather than sending the user off to run something else first.
     *
     * ⚠️ Only while the sensor is PARKED. A running one has its measurement
     * task on that bus, and reading the OTP from the CLI thread would cut into
     * one of its transactions. */
    if (!s->otp.read) {
        if (s->running) {
            CLI_PRINTF_N("lux %s: OTP not read yet, and the sensor is running -"
                         " stop it to read it now", s->label);
            return;
        }
        if (!s->up) {
            CLI_PRINTF_N("lux %s: no working I2C%u", s->label, s->bus);
            return;
        }

        /* Identify first: a NACK here is wiring or address, and reading the
         * OTP of something that did not answer would report nonsense. */
        u8 id[2] = { 0, 0 };
        if (!vd_read_regs(s->bus, VD6283_DEVICE_ID, id, 2u)) {
            CLI_PRINTF_N("lux %s: no answer on I2C%u - check wiring and pull-ups",
                         s->label, s->bus);
            return;
        }
        s->dev_id = id[0];
        s->rev_id = id[1];
        if (id[0] != VD6283_DEV_ID_VAL) {
            CLI_PRINTF_N("lux %s: wrong DEVICE_ID 0x%02X (expected 0x%02X)",
                         s->label, s->dev_id, VD6283_DEV_ID_VAL);
            return;
        }
        vd_read_otp(s);
    }

    if (!s->otp.read) {
        /* The read was attempted just above (or at start-up) and failed: the
         * part answers but its OTP does not come back. Say so plainly - these
         * are the fallback values, not measurements of this die. */
        CLI_PRINTF_N("lux %s: OTP could NOT be read - running on nominal defaults",
                     s->label);
        CLI_PRINTF_N("  hf_trim=0x%03X lf_trim=0x%X (defaults), all gains 0x%02X",
                     (u32)s->otp.hf_trim, (u32)s->otp.lf_trim, VD6283_DEFAULT_GAIN);
        CLI_PRINTF_N("  readings are repeatable but NOT comparable between parts");
        return;
    }
    /* The UID names the DIE. "A" and "B" are bus positions: the two parts were
     * found swapped between one session and the next, which silently moved
     * every per-part coefficient to the other label. Quote this, not A/B, when
     * a measurement is attributed to a sensor. */
    CLI_PRINTF_N("lux %s: OTP v0x%02X  uid=%s",
                 s->label, (u32)s->otp.version, s->otp.uid);
    CLI_PRINTF_N("  hf_trim : 0x%03X   (default 0x%03X)",
                 (u32)s->otp.hf_trim, (u32)VD6283_HF_TRIM);
    CLI_PRINTF_N("  lf_trim : 0x%X     (default 0x%X)",
                 (u32)s->otp.lf_trim, (u32)VD6283_LF_TRIM);
    CLI_PRINTF_N("  correction %s", s->otp_on ? "APPLIED" : "off");

    /* ⚠️ Each OTP byte holds TWO coefficients, not one: the high nibble is the
     * calibration for gains below 33.33x and the low nibble the one at or above
     * it (calfactor1050() in the ST driver, split at GAIN_CAL_SPLIT). Printing
     * only the one in force made the table look like a per-channel constant,
     * and a reading taken after the autogain crossed the split was corrected by
     * a coefficient the console had never shown. Both are listed, with "<-" on
     * whichever the CURRENT gain selects. */
    {
        const u16 g   = GAIN_VALUE[s->gain_idx];
        const bool hi = (g < GAIN_CAL_SPLIT);    /* true => high nibble in use */
        u32 gw, gf; gain_text(s->gain_idx, &gw, &gf);

        CLI_PRINTF_N("  channel   byte    <33.33x   >=33.33x   (gain now %u.%02ux)",
                     gw, gf);
        for (u8 c = 0; c < ALS_CHANNELS; c++) {
            /* Ask for each nibble by handing otp_calfactor a gain on either
             * side of the split, so the mapping is never duplicated here. */
            u32 lo_m = ((u32)otp_calfactor(s->otp.gains[c], 0x0100u) * 1000u) / 128u;
            u32 hi_m = ((u32)otp_calfactor(s->otp.gains[c], 0x42ABu) * 1000u) / 128u;
            CLI_PRINTF_N("  %-5s   : 0x%02X   x%u.%03u%s  x%u.%03u%s",
                         CH_NAME[c], (u32)s->otp.gains[c],
                         lo_m / 1000u, lo_m % 1000u, hi ? " <-" : "   ",
                         hi_m / 1000u, hi_m % 1000u, hi ? "" : " <-");
        }
    }
}

/* Close the menu and say so, so the console never looks stuck in a mode. */
static bool menu_quit(void)
{
    s_menu = MENU_OFF;
    CLI_PRINTF_N("lux setup: closed");
    return true;
}

/*
 * Handle one line while a menu is open. Returns true always - the line belongs
 * to the menu, not to the command set.
 */
static bool menu_input(const char *line)
{
    /* The menu is bound to ONE context, but every entry acts on both sensors:
     * `s` is only where the currently displayed values are read from. Entry 7
     * used to pass its index to otp_report() and so reported a single part -
     * it now loops, and with it went the last use of that index. */
    sensor_ctx_t *s = &s_sensor[s_menu_s < SENSOR_COUNT ? s_menu_s : 0];
    const char   *v = ca_skip_sp(line);

    /* An empty line always backs out one level, and "q" quits outright. Both
     * work from every state: there is no way to get trapped in the menu. */
    if (!*v) {
        if (s_menu == MENU_TOP) return menu_quit();
        s_menu = MENU_TOP;                  /* cancel this prompt */
        menu_show(s);
        return true;
    }
    if (*v == 'q' || *v == 'Q') return menu_quit();

    /* "help" reaches here like any other line - an open menu claims them all -
     * so it has to be answered here too. Without this it fell through to the
     * numeric parse, where ca_strtol("help") is 0 and the reply was the
     * useless "no entry 0". Works from a value prompt as well, where knowing
     * what is being asked for matters most. */
    if (*v == '?' || *v == 'h' || *v == 'H') {
        CLI_PRINTF_N("");
        CLI_PRINTF_N("lux setup menu (both sensors):");
        CLI_PRINTF_N("  <number>  pick an entry from the list");
        CLI_PRINTF_N("  <empty>   cancel the current question, or quit the menu");
        CLI_PRINTF_N("  q         quit the menu at once");
        CLI_PRINTF_N("  h or ?    this help");
        CLI_PRINTF_N("");
        CLI_PRINTF_N("The menu configures BOTH sensors at once: this application");
        CLI_PRINTF_N("compares them, and different settings would make it compare");
        CLI_PRINTF_N("configurations instead. It opens only while both are stopped.");
        CLI_PRINTF_N("The autogain is the exception - it reacts to the light each");
        CLI_PRINTF_N("sensor sees, so GAIN and EXP can diverge once it is on.");
        CLI_PRINTF_N("");
        CLI_PRINTF_N("Commands available outside the menu:");
        print_commands();
        if (s_menu == MENU_TOP) menu_show(s);
        else CLI_PRINTF_N("(still waiting for your answer - empty line to cancel)");
        return true;
    }

    switch (s_menu) {
    case MENU_ASK_EXP: {
        i32 ms = ca_strtol(v, 10);
        if (ms < (i32)EXTIME_MS_MIN || ms > (i32)EXTIME_MS_MAX) {
            CLI_PRINTF_N("exposure must be %u..%u ms - try again (empty to cancel):",
                         EXTIME_MS_MIN, EXTIME_MS_MAX);
            return true;                    /* stay in the prompt */
        }
        /* ⚠️ The INVERSE of STEPS_TO_MS, and it has to stay its inverse: this
         * used to subtract 1 to undo the "+1" that STEPS_TO_MS carried, and
         * both halves moved together when the convention was corrected (see
         * EXTIME_PLUS_ONE). Mixing the two conventions made the round trip
         * drift - asking for 400 ms produced EXTIME 250 and exposure_ms() then
         * reported 401 - which is why the -1 is gone from HERE too and not
         * only from the forward direction. */
        u32 st = ((u32)ms * 1000u + (EXTIME_US_PER_STEP / 2u)) / EXTIME_US_PER_STEP;
        st = (st > EXTIME_PLUS_ONE) ? (st - EXTIME_PLUS_ONE) : 0u;
        if (st < EXTIME_STEPS_MIN) st = EXTIME_STEPS_MIN;
        if (st > EXTIME_STEPS_MAX) st = EXTIME_STEPS_MAX;
        setup_all(SET_EXP, st);
        CLI_PRINTF_N("exposure -> %u ms (EXTIME=0x%03X), ~%u ms per sample"
                     "  [both sensors]",
                     exposure_ms(s), st, SAMPLE_PERIOD_MS(s));
        break;
    }
    case MENU_ASK_GAIN: {
        /* Decimal index here, not the hex register code: the menu prints the
         * indices next to the gains, so the answer is what was just shown. */
        i32 g = ca_strtol(v, 10);
        if (g < 0 || g >= (i32)GAIN_TABLE_SIZE) {
            CLI_PRINTF_N("index must be 0..%u - try again (empty to cancel):",
                         GAIN_TABLE_SIZE - 1u);
            return true;
        }
        setup_all(SET_GAIN, (u32)g);
        u32 gw, gf; gain_text(s->gain_idx, &gw, &gf);
        CLI_PRINTF_N("gain -> %u.%02ux (register code 0x%02X)  [both sensors]",
                     gw, gf, (u32)(g + 1));
        break;
    }
    case MENU_ASK_OTP:
    case MENU_ASK_AUTO:
    case MENU_ASK_DARK: {
        bool on;
        if (ca_strncmp(v, "on", 2) == 0 || *v == '1')       on = true;
        else if (ca_strncmp(v, "off", 3) == 0 || *v == '0') on = false;
        else { CLI_PRINTF_N("answer on or off (empty to cancel):"); return true; }
        if (s_menu == MENU_ASK_OTP) {
            setup_all(SET_OTP, on ? 1u : 0u);
            CLI_PRINTF_N("factory correction -> %s%s  [both sensors]",
                         on ? "on" : "off",
                         (on && !s->otp.read)
                             ? "  (but the OTP of this one was never read)" : "");
        } else if (s_menu == MENU_ASK_AUTO) {
            setup_all(SET_AUTO, on ? 1u : 0u);
            CLI_PRINTF_N("autogain -> %s  [both sensors]",
                         on ? "on (clear channel, 40-85% FS)" : "off");
            if (on)
                CLI_PRINTF_N("  each sensor then corrects for the light IT sees,"
                             " so gain/exp may diverge");
        } else {
            /* Both sensors, like every other menu setting: the dark reference
             * is only meaningful if the two are read the same way. A sensor
             * that is running gets the request handed to its task, which
             * applies it at a frame boundary; a parked one is written here. */
            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                sensor_ctx_t *t = &s_sensor[i];
                t->dark_req = on;
                if (t->running) continue;               /* its task will do it */
                if (!vd_set_dark_mode(t, on))
                    CLI_PRINTF_N("sensor %s: SEL_PD write failed - unchanged",
                                 t->label);
            }
            CLI_PRINTF_N("dark mode -> %s%s  [both sensors]", on ? "on" : "off",
                         on ? "  (channel 2 reads DARK, no vis)" : "");
        }
        break;
    }

    case MENU_TOP:
    default: {
        i32 sel = ca_strtol(v, 10);
        switch (sel) {
        case MI_EXP:
            CLI_PRINTF_N("exposure in ms, %u..%u (now %u, empty to cancel):",
                         EXTIME_MS_MIN, EXTIME_MS_MAX,
                         exposure_ms(s));
            s_menu = MENU_ASK_EXP;
            return true;
        case MI_GAIN:
            /* List the table: a bare index means nothing without it. */
            CLI_PRINTF_N("gain index (now %u, empty to cancel):", (u32)s->gain_idx);
            for (u8 i = 0; i < GAIN_TABLE_SIZE; i++) {
                u32 gw, gf; gain_text(i, &gw, &gf);
                CLI_PRINTF_N("   %2u) %u.%02ux%s", (u32)i, gw, gf,
                             i == s->gain_idx ? "   <- current" : "");
            }
            s_menu = MENU_ASK_GAIN;
            return true;
        case MI_OTP:
            CLI_PRINTF_N("factory correction on or off (now %s, empty to cancel):",
                         s->otp_on ? "on" : "off");
            s_menu = MENU_ASK_OTP;
            return true;
        case MI_AUTO:
            CLI_PRINTF_N("autogain on or off (now %s, empty to cancel):",
                         s->auto_gain ? "on" : "off");
            s_menu = MENU_ASK_AUTO;
            return true;
        case MI_DARK:
            CLI_PRINTF_N("dark mode on or off (now %s, empty to cancel):",
                         s->dark_mode ? "on" : "off");
            CLI_PRINTF_N("  on = channel 2 moves to the covered photodiodes;");
            CLI_PRINTF_N("  it must NOT follow the light - that is the point.");
            s_menu = MENU_ASK_DARK;
            return true;
        case MI_RESET:
            setup_all(SET_DEFAULTS, 0u);
            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                s_sensor[i].dark_req = false;
                (void)vd_set_dark_mode(&s_sensor[i], false);
            }
            {
                u32 gw, gf; gain_text(s->gain_idx, &gw, &gf);
                CLI_PRINTF_N("reset: %u ms, gain %u.%02ux, otp on, autogain off,"
                             " dark off  [both sensors]",
                             exposure_ms(s), gw, gf);
            }
            break;
        case MI_SHOW:
            /* ⚠️ BOTH parts, not the one the menu happens to be bound to.
             * Every other entry in this menu acts on both, and the OTP is the
             * one thing that genuinely DIFFERS between the two dies - showing
             * a single one here was exactly backwards. It reported sensor A
             * whatever the user had selected. */
            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                if (i) CLI_PRINTF_N("");
                otp_report(&s_sensor[i], (int)i + 1);
            }
            break;
        case MI_QUIT:
            return menu_quit();
        default:
            CLI_PRINTF_N("no entry \"%s\" - pick 1-%d, h for help, q to quit",
                         v, MI_QUIT);
            return true;                    /* redisplay would scroll the list away */
        }
        break;
    }
    }

    /* An action completed: back to the list, with the new value showing. */
    s_menu = MENU_TOP;
    menu_show(s);
    return true;
}

/* Start or stop one sensor, with the reason printed when it cannot happen.
 * Factored out so "lux start" (both) and "lux start 1" (one) cannot drift
 * apart - two copies of this would, at the first change to either. */
static void lux_start_one(u8 idx)
{
    sensor_ctx_t *s = &s_sensor[idx];
    u32 n = (u32)idx + 1u;

    if (s->running) {
        CLI_PRINTF_N("lux: sensor %u (%s) is already running", n, s->label);
        return;
    }
    if (!s->up) {
        CLI_PRINTF_N("lux: sensor %u (%s) has no working I2C%u - not started",
                     n, s->label, s->bus);
        return;
    }
    /* ⚠️ "lux start" DOES NOT CALIBRATE ANY MORE. It used to arm k by itself on
     * the first run of a session, which was a convenience under a lit bench and
     * a trap everywhere else - and this application spends its life in a closed
     * box.
     *
     * In darkness R+G+B is 0, so the accumulator's "rgb > 0" guard is never
     * satisfied, cal_req is never decremented, and the calibration NEVER ENDS.
     * That is not merely a wasted preamble: while cal_req is non-zero the
     * autogain is held off (see the ag_update call site), so the sensor stays
     * pinned at whatever gain it started with - unable to reach the settings
     * that would have given it something to calibrate WITH. Seen on HW
     * 2026-09-02: sixteen-plus rows of zeros at 1.00x, every row marked CAL,
     * the gain never moving, and no message saying why.
     *
     * k is now measured only when it is asked for, by "lux cal" - which is
     * where a calibration belongs anyway: it needs a known, steady light, and
     * "lux start" cannot know it has one. Until then cal_k stays 0, the CVIS
     * column reads "--" and the banner says "k not measured yet", all of which
     * is true and none of which stops an acquisition. */

    /* The task picks this up at its next poll, at most IDLE_POLL_MS from now. */
    s->running = true;
    CLI_PRINTF_N("lux: sensor %u (%s) started on I2C%u", n, s->label, s->bus);
    CLI_PRINTF_N("     first sample in ~%u ms", exposure_ms(s) + CONV_TIME_MS);
}

static void lux_stop_one(u8 idx)
{
    sensor_ctx_t *s = &s_sensor[idx];
    u32 n = (u32)idx + 1u;

    if (!s->running) {
        CLI_PRINTF_N("lux: sensor %u (%s) is not running", n, s->label);
        return;
    }
    /* Just clear the flag. The task notices it at the top of its next
     * iteration, finishes the frame it is in, disables the ALS engine itself
     * and goes back to polling. The CLI never touches the I2C bus of a running
     * sensor, so a stop can never cut a transaction in half. */
    s->running = false;
    CLI_PRINTF_N("lux: sensor %u (%s) stopping, up to %u ms to settle",
                 n, s->label, exposure_ms(s) + CONV_TIME_MS);

    /* Leaving the run leaves the mixed acquisition: it is a property of a
     * capture, not of the configuration. This is the ONLY way out - there is
     * deliberately no "lux meas off" to disagree with it. div and Kdark are
     * kept, so re-arming the same method is "lux meas" with the same numbers,
     * and "lux meas" with none reports what they were. */
    if (s->meas_on) {
        s->meas_on = false;
        CLI_PRINTF_N("     mixed acquisition off (div=%u Kdark=%u.%03u kept)",
                     (u32)s->meas_div, s->k_dark / K_DARK_SCALE,
                     s->k_dark % K_DARK_SCALE);
    }
    s->dark_hold_ok = false;    /* a held dark does not survive a stop */
}

static bool user_cli_handler(c_str *pcmd)
{
    c_str       cmd = *pcmd;
    const char *p   = ca_skip_sp(cmd->data);

    /* An open menu owns the line before any command is considered. Nothing
     * else can run while it is up, which is what makes "1" unambiguous. */
    if (s_menu != MENU_OFF) return menu_input(p);

    if (ca_strncmp(p, "show", 4) == 0) {
        /* Print both forms: the datasheet quotes 0x40 (the 8-bit write byte)
         * while the caOS API takes the 7-bit address, and seeing only one of
         * the two is exactly what makes people suspect the wrong thing. */
        CLI_PRINTF_N("VD6283TX x2 - address 0x%02X 7-bit (= 0x%02X write / 0x%02X read)",
                     VD6283_ADDR, VD6283_ADDR << 1, (VD6283_ADDR << 1) | 1u);
        CLI_PRINTF_N("  readout    : 24-bit, continuous, period 0");
        CLI_PRINTF_N("  queue      : %u/%u items", ca_queue_static_count(&s_queue.q), Q_DEPTH);
        CLI_PRINTF_N("  printed    : %u", s_printed);
        for (u8 i = 0; i < SENSOR_COUNT; i++) {
            const sensor_ctx_t *s = &s_sensor[i];
            /* "present" only becomes true once the task has run and probed the
             * part, so an idle sensor must not be reported as missing. */
            CLI_PRINTF_N("  sensor %s   : %s  (lux %s %u)", s->label,
                         !s->up      ? "I2C init FAILED" :
                         !s->running ? "stopped" :
                         s->present  ? "running" :
                         s->fail == FAIL_NO_ACK ? "no answer on the bus" :
                         s->fail == FAIL_BAD_ID ? "wrong DEVICE_ID" :
                         s->fail == FAIL_CONFIG ? "config write failed" :
                                                  "starting",
                         s->running ? "stop" : "start", (u32)(i + 1u));
            CLI_PRINTF_N("    %s", s->pins);
            if (s->fail == FAIL_BAD_ID)
                CLI_PRINTF_N("    read id=0x%02X rev=0x%02X, expected id=0x%02X",
                             s->dev_id, s->rev_id, VD6283_DEV_ID_VAL);
            {   /* Acquisition parameters per sensor: with the autogain on
                 * these differ between the two and drift over time, so a
                 * single global line at the top would be wrong for both. */
                u32 gw, gf; gain_text(s->gain_idx, &gw, &gf);
                CLI_PRINTF_N("    exp=%ums period=%ums (~%ums/sample)"
                             " gain=%u.%02ux otp=%s auto=%s",
                             exposure_ms(s), IM_STEPS_TO_MS(s->im_steps),
                             SAMPLE_PERIOD_MS(s), gw, gf,
                             !s->otp.read ? "n/a" : s->otp_on ? "on" : "off",
                             s->auto_gain ? "on" : "off");
            }
            if (s->meas_on)
                CLI_PRINTF_N("    meas: on %s, 1 dark every %u, Kdark=%u.%03u,"
                             " discard=%u, held=%s",
                             (s->meas_ch == MEAS_CH_CLEAR) ? "CLEAR" : "VIS",
                             (u32)s->meas_div, s->k_dark / K_DARK_SCALE,
                             s->k_dark % K_DARK_SCALE, (u32)s->meas_discard,
                             s->dark_hold_ok ? "yes" : "not yet");
            CLI_PRINTF_N("    woke=%u samples=%u errors=%u dropped=%u adjusts=%u",
                         s->woke, s->samples, s->errors, s->dropped, s->adjusts);
            if (s->frozen > 2u)
                CLI_PRINTF_N("    ^ %u identical frames: the part is not refreshing",
                             s->frozen);
        }
        return true;
    }

    if (ca_strncmp(p, "lux", 3) == 0) {
        const char *sub = ca_skip_sp(p + 3);

        /* Sensors are 1-based on the command line, 0-based in the array.
         * ca_strtol returns 0 on a malformed string, which the range check
         * below rejects along with a genuine "0". */
        bool is_start = (ca_strncmp(sub, "start", 5) == 0);
        bool is_stop  = (ca_strncmp(sub, "stop",  4) == 0);
        bool is_otp   = (ca_strncmp(sub, "otp",   3) == 0);
        bool is_setup = (ca_strncmp(sub, "setup", 5) == 0);
        bool is_dark  = (ca_strncmp(sub, "dark",  4) == 0);
        bool is_probe = (ca_strncmp(sub, "probe", 5) == 0);
        bool is_bytes = (ca_strncmp(sub, "bytes", 5) == 0);

        /* "lux cal" calibrates BOTH, and does the whole sequence itself:
         * start, measure k over CAL_SAMPLES with channel 2 on VIS, restore the
         * dark setting the user had, and stop again. Making the operator
         * remember start/dark off/cal/stop is ceremony, and getting the order
         * wrong silently produces a k measured against the wrong channel.
         *
         * Always RE-calibrates, even when k is already known: that is what the
         * command is for - and since "lux start" stopped calibrating on its
         * own (see lux_start_one), it is also the ONLY way k is ever measured.
         *
         * ⚠️ It needs a LIT bench. k is a ratio between colour channels, so in
         * darkness it degenerates: with R, G, B and VIS all carrying nothing
         * but dark current it converges on 1/3 regardless of the light it is
         * supposed to describe - measured 0.374 on HW in a closed box, against
         * ~1.07 under real light. Below the light level where R+G+B reaches
         * zero it does not converge at all, and CAL_SKIP_MAX is what stops it
         * waiting for ever. */
        if (ca_strncmp(sub, "cal", 3) == 0) {
            /* ⚠️ Refused, not silently useless. k reconstructs VIS from R+G+B
             * for the case where channel 2 is on the dark diodes; the mixed
             * acquisition measures VIS directly, so calibrating here would
             * spend CAL_SAMPLES frames on a coefficient nothing reads. */
            if (s_sensor[0].meas_on || s_sensor[1].meas_on) {
                CLI_PRINTF_N("lux: the mixed acquisition MEASURES its signal"
                             " channel (%s) - k is not used and not needed",
                             (s_sensor[0].meas_ch == MEAS_CH_CLEAR) ? "CLEAR"
                                                                    : "VIS");
                CLI_PRINTF_N("     \"lux stop\" leaves the mode, then \"lux cal\"");
                return true;
            }

            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                sensor_ctx_t *t = &s_sensor[i];

                if (!t->up) {
                    CLI_PRINTF_N("lux %s: no working I2C%u - cannot calibrate",
                                 t->label, t->bus);
                    continue;
                }
                if (t->cal_req) {
                    CLI_PRINTF_N("lux %s: already calibrating", t->label);
                    continue;
                }

                /* Calibration needs a MEASURED vis on channel 2, so dark comes
                 * off for the duration and goes back to whatever it was. */
                t->cal_dark_was = t->dark_req;
                t->dark_req     = false;

                t->cal_acc = 0; t->cal_n = 0; t->cal_sat = 0;
                t->cal_req = (u8)CAL_SAMPLES;

                /* Start it only if it was parked - and remember, so a sensor
                 * the user had already running is left running. */
                if (!t->running) {
                    t->cal_stop = true;
                    t->running  = true;
                } else {
                    t->cal_stop = false;
                }
            }

            CLI_PRINTF_N("lux: measuring k = VIS/(R+G+B) over %u samples",
                         CAL_SAMPLES);
            CLI_PRINTF_N("     !! NEEDS A LIT BENCH. In the dark k collapses to"
                         " 1/3 (measured 0.374 in a");
            CLI_PRINTF_N("     closed box against ~1.07 under real light), and"
                         " below that it does not");
            CLI_PRINTF_N("     converge at all - the command gives up after %u"
                         " unusable samples.", CAL_SKIP_MAX);
            CLI_PRINTF_N("     hold the light STEADY - k follows the spectrum");
            CLI_PRINTF_N("     ~%u s, then the sensors park again",
                         (CAL_SAMPLES * (exposure_ms(&s_sensor[0]) + CONV_TIME_MS)
                          + 999u) / 1000u);
            return true;
        }

        /* ── "lux meas" / "lux meas_cl" — the mixed signal/DARK acquisition ──
         *
         * Arms BOTH sensors, like "lux dark" and "lux setup": the two are only
         * worth comparing when they run the same method.
         *
         * The two spellings differ in ONE thing, the channel the dark is
         * subtracted from: "meas" corrects VIS (channel 2), "meas_cl" corrects
         * CLEAR (channel 6), which has five photodiodes against three and no
         * filter at all - the widest detector on the part. Everything else -
         * div, Kdark, discard, the forced refresh, the row - is shared, so the
         * two are one block rather than two that drift apart.
         *
         * ⚠️ "meas_cl" MUST be tested first: ca_strncmp(sub, "meas", 4) matches
         * the longer word too, so the order of these two tests is the whole
         * dispatch. Reversed, "lux meas_cl" would silently arm the VIS mode and
         * report success.
         *
         * ⚠️ Kdark does NOT carry over between the two. In VIS mode signal and
         * dark share channel 2; in CLEAR mode they do not, and the coefficient
         * has to absorb the mismatch between two covered photodiodes and five
         * clear ones. Re-measure it after switching - see MEAS_CH_CLEAR.
         *
         * There is no "lux meas off". The mode belongs to a run, not to the
         * configuration, so "lux stop" leaves it - one way out instead of two
         * that can disagree about what the sensor is doing.
         *
         * ⚠️ Arming does NOT start the sensors. Entering the mode and beginning
         * a capture are separate decisions, exactly as they are for "lux cal":
         * "lux meas 10 1000" then "lux start". */
        bool is_meas_cl = (ca_strncmp(sub, "meas_cl", 7) == 0);
        if (is_meas_cl || ca_strncmp(sub, "meas", 4) == 0) {
            const u8    mch  = is_meas_cl ? (u8)MEAS_CH_CLEAR : (u8)MEAS_CH_VIS;
            const char *mname = is_meas_cl ? "meas_cl" : "meas";
            const char *mchn  = is_meas_cl ? "CLEAR" : "VIS";
            const char *marg = ca_skip_sp(sub + (is_meas_cl ? 7u : 4u));

            if (!*marg) {
                for (u8 i = 0; i < SENSOR_COUNT; i++) {
                    const sensor_ctx_t *t = &s_sensor[i];
                    /* Reports the channel the sensor is actually armed on,
                     * not the one this invocation named: "lux meas_cl" with no
                     * arguments is a query, and answering it with its own
                     * spelling instead of the mode in force would hide exactly
                     * the mistake it is asked about. */
                    CLI_PRINTF_N("lux %s: meas %s  chan=%s  div=%u  Kdark=%u.%03u"
                                 "  discard=%u",
                                 t->label, t->meas_on ? "ON" : "off",
                                 (t->meas_ch == MEAS_CH_CLEAR) ? "CLEAR" : "VIS",
                                 (u32)t->meas_div,
                                 t->k_dark / K_DARK_SCALE,
                                 t->k_dark % K_DARK_SCALE,
                                 (u32)t->meas_discard);
                }
                CLI_PRINTF_N("usage: lux %s <div> [discard]   (both sensors)",
                             mname);
                CLI_PRINTF_N("       div     = signal samples between two dark reads");
                CLI_PRINTF_N("       Kdark is NOT an argument here: it is"
                             " per-part, see \"lux kdark\"");
                CLI_PRINTF_N("       discard = frames binned after each SEL_PD switch,"
                             " 0..%u (default %u)", MEAS_DISCARD_MAX,
                             MEAS_SWITCH_DISCARD);
                CLI_PRINTF_N("       N%s = norm(%s - Kdark*dark); lux stop leaves the mode",
                             is_meas_cl ? "CLR" : "VIS", mchn);
                CLI_PRINTF_N("       meas    = VIS (ch2, 3 diodes, IR-filtered)");
                CLI_PRINTF_N("       meas_cl = CLEAR (ch6, 5 diodes, no filter) -"
                             " wider band, larger count");
                CLI_PRINTF_N("       Kdark does NOT carry over between the two");
                return true;
            }

            u16 mate = 0;
            i32 mdiv = ca_strtol_index(marg, 10, &mate);
            if (mate == 0u || mdiv < (i32)MEAS_DIV_MIN || mdiv > (i32)MEAS_DIV_MAX) {
                CLI_PRINTF_N("lux: div must be %u..%u (visible samples per dark read)",
                             MEAS_DIV_MIN, MEAS_DIV_MAX);
                return true;
            }

            /* ⚠️ THERE IS NO Kdark ARGUMENT ANY MORE. It was mandatory, then
             * optional, and both were wrong once "lux kdark cal" could measure
             * the coefficient per part: a positional argument in the middle of
             * this command could only ever set BOTH sensors to one value, and
             * it sat exactly where a reader expects the discard.
             *
             * That is not a hypothetical. On HW 2026-09-02 "lux meas_cl 1 0 3"
             * was typed to test the discard and silently zeroed two measured
             * calibrations, because 0 landed in the Kdark slot. The capture
             * that came back was a different experiment from the one intended,
             * and nothing in it said so.
             *
             * Kdark is set by "lux kdark <1|2> <value>" and measured by
             * "lux kdark cal". This command no longer touches it.
             *
             * So: "lux meas <div> [discard]", two arguments at most. */
            marg = ca_skip_sp(marg + mate);
            mate = 0;
            i32 mdisc = (i32)s_sensor[0].meas_discard;
            if (*marg) {
                mdisc = ca_strtol_index(marg, 10, &mate);
                if (mate == 0u || mdisc < 0 || mdisc > (i32)MEAS_DISCARD_MAX) {
                    CLI_PRINTF_N("lux: discard must be 0..%u", MEAS_DISCARD_MAX);
                    return true;
                }
            }

            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                sensor_ctx_t *t = &s_sensor[i];
                t->meas_div     = (u16)mdiv;
                t->meas_discard = (u8)mdisc;
                /* k_dark is deliberately NOT touched here - see above. */
                t->meas_ch      = mch;
                t->meas_on      = true;
                /* The mode owns SEL_PD: between dark reads channel 2 must be on
                 * the VISIBLE diodes. In VIS mode that is where the signal is
                 * read from; in CLEAR mode it is not, but channel 2 must still
                 * come back off the covered diodes or the NEXT dark read would
                 * find it already there and measure nothing new. Either way the
                 * resting state is the same one. A running task applies this at
                 * its next frame; a parked one gets it from vd_configure(). */
                t->dark_req       = false;
                /* Nothing held is trustworthy across an arming: div and Kdark
                 * have just changed, and so may the settings since the last
                 * dark. The first sample of the new mode gets a fresh one. */
                t->dark_hold_ok   = false;
                t->dark_forced    = true;
                t->vis_since_dark = 0;
            }

            {
                u32 ms = SAMPLE_PERIOD_MS(&s_sensor[0]);
                CLI_PRINTF_N("lux: mixed acquisition armed on both sensors,"
                             " correcting %s", mchn);
                CLI_PRINTF_N("     1 dark every %d %s samples", (int)mdiv, mchn);
                for (u8 i = 0; i < SENSOR_COUNT; i++)
                    CLI_PRINTF_N("     %s: Kdark=%u.%03u  (per-part, set by"
                                 " \"lux kdark\")", s_sensor[i].label,
                                 s_sensor[i].k_dark / K_DARK_SCALE,
                                 s_sensor[i].k_dark % K_DARK_SCALE);
                if (is_meas_cl)
                    CLI_PRINTF_N("     Kdark measured for VIS does NOT apply here:"
                                 " 2 covered diodes vs 5 clear");

                /* ⚠️ Warn at ARMING, not after a run of zeros. This mode works
                 * at the floor, and the part reports nothing below
                 * ALS_MIN_REPORTED - so at low gain with the autogain off it
                 * produces a table of zeros that looks like a fault and is not
                 * one. Caught twice on HW before it was named, both times
                 * straight after "lux kdark cal" put the sensors back on the
                 * settings they had before it borrowed them. */
                for (u8 i = 0; i < SENSOR_COUNT; i++) {
                    const sensor_ctx_t *t = &s_sensor[i];
                    if (t->auto_gain || t->gain_idx <= MEAS_GAIN_IDX_WARN)
                        continue;
                    {
                        u32 gw, gf; gain_text(t->gain_idx, &gw, &gf);
                        CLI_PRINTF_N("     !! %s is at %u.%02ux with autogain OFF"
                                     " - the part reports nothing", t->label, gw, gf);
                        CLI_PRINTF_N("        below %u counts (par 3.3.3), so at"
                                     " the floor every column reads 0.",
                                     ALS_MIN_REPORTED);
                        CLI_PRINTF_N("        Raise the gain, or turn the autogain"
                                     " on in \"lux setup\".");
                    }
                }
                CLI_PRINTF_N("     discard=%d: a dark refresh costs %u frames"
                             " (~%u ms), forced on every gain change",
                             (int)mdisc, 2u * ((u32)mdisc + 1u),
                             2u * ((u32)mdisc + 1u) * ms);
                if (s_sensor[0].running || s_sensor[1].running) {
                    /* The table changes shape mid-capture, so the new column
                     * names have to go out now - the header is otherwise only
                     * printed when a sensor starts. */
                    print_header();
                } else {
                    CLI_PRINTF_N("     now \"lux start\"");
                }
            }
            return true;
        }

        /* ── "lux kdark" — the dark coefficient, per sensor ─────────────────
         *
         *   lux kdark              show both
         *   lux kdark <1|2> <val>  set one, x1000 (1200 = 1.200)
         *   lux kdark cal          MEASURE it on both, in darkness
         *
         * ⚠️ Per SENSOR, unlike every other setting this application exposes.
         * The rest are deliberately global - the two parts are only worth
         * comparing when they run the same configuration - but Kdark is not a
         * configuration, it is a per-part CALIBRATION, in the same family as
         * cal_k. Measured 1.06 on A against 1.33 on B: one shared value cannot
         * be right for both, and 1.200 was wrong for each in opposite
         * directions.
         *
         * The "cal" form is the one to prefer. See KDARK_SAMPLES: it reads
         * CLEAR and DARK out of the same frame, on RAW counts, which is the
         * quantity meas_correct() actually subtracts - a ratio taken by hand
         * off the printed columns carries the two channels\' OTP coefficients
         * and comes out some 7% wrong. */
        if (ca_strncmp(sub, "kdark", 5) == 0) {
            const char *karg = ca_skip_sp(sub + 5);

            if (!*karg) {
                for (u8 i = 0; i < SENSOR_COUNT; i++)
                    CLI_PRINTF_N("lux %s: Kdark=%u.%03u", s_sensor[i].label,
                                 s_sensor[i].k_dark / K_DARK_SCALE,
                                 s_sensor[i].k_dark % K_DARK_SCALE);
                CLI_PRINTF_N("usage: lux kdark <1|2> <value>   set one, x1000"
                             " (1200 = 1.200)");
                CLI_PRINTF_N("       lux kdark cal             measure both,"
                             " IN DARKNESS");
                CLI_PRINTF_N("  Kdark = raw CLEAR / raw DARK, both from the same"
                             " frame with channel 2");
                CLI_PRINTF_N("  on the covered diodes. It is a per-PART number:"
                             " do not copy it between sensors.");
                return true;
            }

            /* ── "lux kdark cal" ───────────────────────────────────────────── */
            if (ca_strncmp(karg, "cal", 3) == 0) {
                /* Refused during the mixed acquisition for the same reason
                 * "lux cal" is: that mode owns SEL_PD and drives channel 2 back
                 * to the visible diodes between dark reads, so it would fight
                 * the dark mode this measurement needs for every frame. */
                if (s_sensor[0].meas_on || s_sensor[1].meas_on) {
                    CLI_PRINTF_N("lux: the mixed acquisition owns channel 2 -"
                                 " \"lux stop\" first, then \"lux kdark cal\"");
                    return true;
                }

                /* ⚠️ Only on PARKED sensors, because this command changes gain
                 * and exposure - and in this application those are written by
                 * vd_configure() at start, exactly like the clamp. Refusing is
                 * the same answer "lux setup" and "lux clamp" give, and for the
                 * same reason: a setting that lands three commands later is how
                 * a capture ends up describing settings it was not taken at. */
                if (s_sensor[0].running || s_sensor[1].running) {
                    CLI_PRINTF_N("lux: this sets gain and exposure itself -"
                                 " \"lux stop\" first, then \"lux kdark cal\"");
                    return true;
                }

                for (u8 i = 0; i < SENSOR_COUNT; i++) {
                    sensor_ctx_t *t = &s_sensor[i];

                    if (!t->up) {
                        CLI_PRINTF_N("lux %s: no working I2C%u - cannot measure",
                                     t->label, t->bus);
                        continue;
                    }
                    if (t->kd_req) {
                        CLI_PRINTF_N("lux %s: already measuring Kdark", t->label);
                        continue;
                    }

                    /* Channel 2 must be on the COVERED diodes for the whole
                     * measurement - the opposite of what "lux cal" needs, and
                     * restored the same way when it finishes. */
                    t->kd_dark_was = t->dark_req;
                    t->dark_req    = true;

                    /* Maximum sensitivity for the duration, restored when it
                     * finishes or gives up. See kd_gain_was: at unity gain the
                     * dark is not merely small, it is zero, and the operator
                     * should not have to know that before running a command
                     * whose whole job is to measure the dark. */
                    t->kd_gain_was = t->gain_idx;   t->gain_idx  = 0u;
                    t->kd_exp_was  = t->exp_steps;  t->exp_steps = EXTIME_STEPS_MAX;
                    t->kd_auto_was = t->auto_gain;  t->auto_gain = false;

                    t->kd_clear = 0; t->kd_dark = 0;
                    t->kd_n = 0; t->kd_skip = 0;
                    t->kd_min = 0; t->kd_max = 0;
                    t->kd_req = (u8)KDARK_SAMPLES;

                    t->kd_stop = true;
                    t->running  = true;
                }

                {
                    u32 gw, gf; gain_text(0u, &gw, &gf);
                    u32 ms = STEPS_TO_MS(EXTIME_STEPS_MAX) + CONV_TIME_MS;
                    CLI_PRINTF_N("lux: measuring Kdark = CLEAR/DARK over %u"
                                 " samples, on RAW counts", KDARK_SAMPLES);
                    CLI_PRINTF_N("     at %u.%02ux / %u ms - the maximum. At"
                                 " 1.00x the dark reads 0 and there is",
                                 gw, gf, STEPS_TO_MS(EXTIME_STEPS_MAX));
                    CLI_PRINTF_N("     nothing to take a ratio of; both settings"
                                 " are put back afterwards.");
                    CLI_PRINTF_N("     !! THE SENSORS MUST BE IN THE DARK. Any"
                                 " light lands on CLEAR and not on the");
                    CLI_PRINTF_N("     covered diodes, so it inflates the ratio."
                                 " At full gain room light lands near");
                    CLI_PRINTF_N("     50 and is refused - but DIM light is not"
                                 " caught, so close the box.");
                    CLI_PRINTF_N("     ~%u s, then the sensors park again",
                                 (KDARK_SAMPLES * ms + 999u) / 1000u);
                }
                return true;
            }

            /* ── "lux kdark <1|2> <value>" ─────────────────────────────────── */
            u16 kate = 0;
            i32 kwho = ca_strtol_index(karg, 10, &kate);
            if (kate == 0u || kwho < 1 || kwho > (i32)SENSOR_COUNT) {
                CLI_PRINTF_N("lux: which sensor? 1..%u, or \"cal\" to measure",
                             SENSOR_COUNT);
                return true;
            }

            karg = ca_skip_sp(karg + kate);
            kate = 0;
            i32 kval = ca_strtol_index(karg, 10, &kate);
            if (kate == 0u || kval < 0 || kval > (i32)K_DARK_MAX) {
                CLI_PRINTF_N("lux: Kdark must be 0..%u, x1000 (1000 = 1.000)",
                             K_DARK_MAX);
                return true;
            }

            {
                sensor_ctx_t *t = &s_sensor[kwho - 1];
                t->k_dark = (u32)kval;
                CLI_PRINTF_N("lux %s: Kdark -> %u.%03u", t->label,
                             t->k_dark / K_DARK_SCALE, t->k_dark % K_DARK_SCALE);
                /* The other sensor is deliberately NOT touched, and saying so
                 * matters: every other setting in this application applies to
                 * both, so silence here would read as "it did both". */
                CLI_PRINTF_N("     the other sensor is unchanged - Kdark is"
                             " per-part");
            }
            return true;
        }

        /* ── "lux period [ms]" — the idle gap between measurements ──────────
         *
         * ALS_PERIOD (§4.3.5). Everything this application has measured so far
         * ran at 0 - back-to-back frames - which is right for a bench and wrong
         * for an always-on alarm: a threshold does not need a reading every
         * 400 ms, and frames not taken are the cheapest power there is.
         *
         * ⚠️ It is the GAP, not the interval: the part integrates for EXTIME,
         * then idles for the period, so the sample rate is set by the sum. That
         * is why this is a separate knob from the exposure - the exposure also
         * changes what each reading MEANS (its saturation ceiling, its noise),
         * and the period changes only how often it is taken.
         *
         * ⚠️ Only on a PARKED sensor, for the same reason as "lux clamp": the
         * register is written by vd_configure(). */
        if (ca_strncmp(sub, "period", 6) == 0) {
            const char *parg = ca_skip_sp(sub + 6);

            if (!*parg) {
                for (u8 i = 0; i < SENSOR_COUNT; i++) {
                    const sensor_ctx_t *t = &s_sensor[i];
                    CLI_PRINTF_N("lux %s: period %u ms (ALS_PERIOD=%u)"
                                 "  -> ~%u ms per sample",
                                 t->label, IM_STEPS_TO_MS(t->im_steps),
                                 (u32)t->im_steps, SAMPLE_PERIOD_MS(t));
                }
                CLI_PRINTF_N("usage: lux period <ms>   (both sensors, applied at next start)");
                CLI_PRINTF_N("       0 = no gap, back-to-back frames (the default)");
                CLI_PRINTF_N("       step %u.%u ms, max %u ms",
                             IM_STEP_US / 1000u, (IM_STEP_US % 1000u) / 100u,
                             IM_STEPS_TO_MS(IM_STEPS_MAX));
                CLI_PRINTF_N("       the gap is ADDED to the exposure, it does not"
                             " replace it");
                return true;
            }

            u16 pate = 0;
            i32 pms  = ca_strtol_index(parg, 10, &pate);
            if (pate == 0u || pms < 0 ||
                (u32)pms > IM_STEPS_TO_MS(IM_STEPS_MAX)) {
                CLI_PRINTF_N("lux: period must be 0..%u ms",
                             IM_STEPS_TO_MS(IM_STEPS_MAX));
                return true;
            }

            if (s_sensor[0].running || s_sensor[1].running) {
                CLI_PRINTF_N("lux: the period is written at configuration time -"
                             " \"lux stop\" first, then set it, then start");
                return true;
            }

            /* Rounded to the nearest step, then reported back as the value that
             * will actually be programmed rather than the one asked for: the
             * step is 20.5 ms, so most requests are not representable and a
             * command that echoed the request would be quietly lying. */
            u32 psteps = IM_MS_TO_STEPS((u32)pms);
            if (psteps > IM_STEPS_MAX) psteps = IM_STEPS_MAX;

            for (u8 i = 0; i < SENSOR_COUNT; i++)
                s_sensor[i].im_steps = (u8)psteps;

            CLI_PRINTF_N("lux: period -> %u ms (ALS_PERIOD=%u) on both sensors,"
                         " from the next start",
                         IM_STEPS_TO_MS(psteps), psteps);
            CLI_PRINTF_N("     ~%u ms per sample at the exposure in force",
                         SAMPLE_PERIOD_MS(&s_sensor[0]));
            return true;
        }

        /* ── "lux lx" — lux and colour temperature, one immediate reading ────
         *
         * The photometric conversion of LUX_MATRIX, applied to a single read
         * taken here and now. This is the number the threshold comparator will
         * eventually compare against, so it exists as its own command first:
         * a comparator fed a quantity nobody has ever looked at is a comparator
         * nobody can debug.
         *
         * ⚠️ It reads R, G and B, so it works in EITHER dark mode - channel 2 is
         * not involved. That is the property that makes it the right input for
         * the alarm: the dark measurement and the light measurement stop
         * competing for the same channel.
         *
         * ⚠️ Uncalibrated in the absolute. See LUX_MATRIX: the coefficients are
         * ST's, measured on ST's optical stack. Ratios and trends are sound,
         * the absolute scale is only as good as the two stacks matching.
         *
         * ⚠️ Like "raw", this reads whatever the part last converted. On a
         * PARKED sensor that is the final frame of the previous run, which may
         * be minutes old - the value is real, its timestamp is not. */
        if (ca_strncmp(sub, "lx", 2) == 0) {
            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                lux_sample_t one = { 0 };
                lux_photo_t  ph;

                if (!s_sensor[i].present) {
                    CLI_PRINTF_N("lux %s: not present", s_sensor[i].label);
                    continue;
                }
                if (!vd_read_channels(&s_sensor[i], &one)) {
                    CLI_PRINTF_N("lux %s: read failed", s_sensor[i].label);
                    continue;
                }
                if (!s_sensor[i].running)
                    CLI_PRINTF_N("lux %s: sensor parked - this is the last frame"
                                 " of the previous run", s_sensor[i].label);

                /* ⚠️ counts[], not raw[]: ST feeds CountValue (post-OTP) to the
                 * normalisation, and the per-channel coefficients differ, so
                 * they do NOT factor out of a three-channel matrix product. */
                if (!lux_photometric(one.counts, one.gain, one.exp_steps, &ph)) {
                    CLI_PRINTF_N("lux %s: no photometric answer -"
                                 " X+Y+Z <= 0 (too dark, or a spectrum the"
                                 " matrix cannot represent)", s_sensor[i].label);
                    CLI_PRINTF_N("       R=%u G=%u B=%u  gain=%u.%02ux exp=%u ms",
                                 one.counts[CH_RED], one.counts[CH_GREEN],
                                 one.counts[CH_BLUE],
                                 one.gain / 256u, ((one.gain % 256u) * 100u) / 256u,
                                 (u32)one.exp_ms);
                    continue;
                }

                CLI_PRINTF_N("lux %s: %u.%03u lx   x=0.%04u y=0.%04u",
                             s_sensor[i].label,
                             ph.lux_milli / 1000u, ph.lux_milli % 1000u,
                             ph.x_ten_k, ph.y_ten_k);
                if (ph.cct_ok)
                    CLI_PRINTF_N("       cct %u K", ph.cct_k);
                else
                    CLI_PRINTF_N("       cct --  (outside McCamy's fit, or on the"
                                 " singular line y=0.1858)");
                CLI_PRINTF_N("       R=%u G=%u B=%u  gain=%u.%02ux exp=%u ms%s",
                             one.counts[CH_RED], one.counts[CH_GREEN],
                             one.counts[CH_BLUE],
                             one.gain / 256u, ((one.gain % 256u) * 100u) / 256u,
                             (u32)one.exp_ms,
                             one.saturated ? "  !! SATURATED" : "");
            }
            CLI_PRINTF_N("lux: absolute scale UNCALIBRATED (ST's matrix, ST's"
                         " optics) - trust ratios, not the number");
            return true;
        }

        /* ── "lux clamp <on|off>" — the DC clamp on channel 2 ────────────────
         *
         * The one control that decides whether the dark channel can carry dark
         * current: with the clamp on, the analog chain removes it before the
         * ADC and the channel reads 0 in darkness. See DC_CLAMP_ALL.
         *
         * ⚠️ Only on a PARKED sensor. The register is written by vd_configure(),
         * so the setting takes effect at the next "lux start" - which is also
         * what keeps it off the bus a running measurement task owns. Refused
         * rather than deferred: a flag that silently applies three commands
         * later is how a capture ends up describing settings it was not taken
         * at. */
        if (ca_strncmp(sub, "clamp", 5) == 0) {
            const char *carg = ca_skip_sp(sub + 5);

            if (!*carg) {
                for (u8 i = 0; i < SENSOR_COUNT; i++)
                    CLI_PRINTF_N("lux %s: channel 2 DC clamp %s%s",
                                 s_sensor[i].label,
                                 s_sensor[i].ch2_clamp ? "on" : "OFF",
                                 s_sensor[i].ch2_clamp ? "" : "  (off the ST driver)");
                CLI_PRINTF_N("usage: lux clamp <on|off>   (both sensors, applied at next start)");
                CLI_PRINTF_N("  on  = DC_CLAMP_EN 0x%02X, what the ST driver writes",
                             DC_CLAMP_ALL);
                CLI_PRINTF_N("  off = 0x%02X: channel 2 unclamped, so its dark current"
                             " survives to the ADC", DC_CLAMP_CH2_OFF);
                return true;
            }

            bool con;
            if      (ca_strncmp(carg, "on",  2) == 0) con = true;
            else if (ca_strncmp(carg, "off", 3) == 0) con = false;
            else { CLI_PRINTF_N("usage: lux clamp <on|off>"); return true; }

            if (s_sensor[0].running || s_sensor[1].running) {
                CLI_PRINTF_N("lux: the clamp is written at configuration time -"
                             " \"lux stop\" first, then set it, then start");
                return true;
            }

            for (u8 i = 0; i < SENSOR_COUNT; i++) s_sensor[i].ch2_clamp = con;

            CLI_PRINTF_N("lux: channel 2 DC clamp %s on both sensors, from the"
                         " next start", con ? "on" : "OFF");
            if (!con)
                CLI_PRINTF_N("     !! off the ST driver: set_output_dark_enable()"
                             " writes SEL_PD only and leaves the clamp on");
            return true;
        }

        /* No sensor number: the header describes the columns, not a part. */
        if (ca_strncmp(sub, "head", 4) == 0) { print_header(); return true; }

        if (!is_start && !is_stop && !is_otp && !is_setup && !is_dark && !is_probe
            && !is_bytes) {
            /* "opt" is the transposition everyone makes - OTP is One-Time
             * Programmable, not "option". Naming the mistake beats repeating
             * the usage: an unchanged usage block gives no clue WHICH word was
             * wrong, so the same typo just gets retyped. */
            if (ca_strncmp(sub, "opt", 3) == 0)
                CLI_PRINTF_N("lux: did you mean \"otp\"? (One-Time Programmable)");
            else
                CLI_PRINTF_N("lux: unknown subcommand");
            CLI_PRINTF_N("usage: lux start [1|2] | lux stop [1|2]   (no number = both)");
            CLI_PRINTF_N("       lux otp <1|2>   | lux setup   (both sensors)");
            CLI_PRINTF_N("       lux dark <on|off>  (both)  | lux probe <1|2> [ch]");
            CLI_PRINTF_N("       lux bytes <1|2>  (raw channel bytes, diagnostic)");
            CLI_PRINTF_N("       lux head  (reprint the column header)");
            CLI_PRINTF_N("       lux cal   (calibrate VIS = k * (R+G+B))");
            CLI_PRINTF_N("       lux meas <div> <Kdark> [discard]  (mixed VIS/DARK)");
            CLI_PRINTF_N("       lux clamp <on|off>  (DC clamp on channel 2)");
            return true;
        }

        u32 skip = is_start ? 5u : is_stop ? 4u : is_otp ? 3u : is_dark ? 4u
                 : is_probe ? 5u : is_bytes ? 5u : 5u;
        const char *arg = ca_skip_sp(sub + skip);

        /* "lux start" / "lux stop" with no number act on BOTH, to match the
         * common setup: the usual thing to want is the pair running under the
         * same settings, and naming each one in turn is just ceremony. */
        if ((is_start || is_stop) && !*arg) {
            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                if (is_start) lux_start_one(i);
                else          lux_stop_one(i);
            }
            return true;
        }

        u16 ate = 0;
        i32 n   = ca_strtol_index(arg, 10, &ate);

        /* ⚠️ "lux setup" and "lux dark" configure BOTH sensors, so the number
         * selects nothing - it survives only because the older syntax had it.
         * Both forms must work:
         *      lux dark on        lux dark 1 on
         *      lux setup          lux setup 1
         * ca_strtol_index returns 0 with ate=0 when there is no number, so a
         * missing one is told apart from a bad one by how much it consumed -
         * not by the value, which would make "lux dark 0 on" look valid. */
        if ((is_setup || is_dark) && ate == 0u) {
            n = 1;                      /* any index: the action covers both */
        } else if (n < 1 || n > (i32)SENSOR_COUNT) {
            CLI_PRINTF_N("lux: sensor must be 1 or 2");
            return true;
        }

        sensor_ctx_t *s = &s_sensor[n - 1];
        const char *opt = ca_skip_sp(arg + ate);

        if (is_probe) {
            i32 ch = *opt ? ca_strtol(opt, 10) : 1;   /* channel 2 by default */
            if (ch < 0 || ch >= (i32)ALS_CHANNELS) {
                CLI_PRINTF_N("lux: channel must be 0..%u (0=red 1=vis 2=blue"
                             " 3=green 4=IR 5=clear)", ALS_CHANNELS - 1u);
                return true;
            }
            if (s->probe_req) {
                CLI_PRINTF_N("lux %s: a sweep is already queued", s->label);
                return true;
            }
            if (!s->up) {
                CLI_PRINTF_N("lux %s: no working I2C%u", s->label, s->bus);
                return true;
            }

            s->probe_req = (u8)(ch + 1);

            /* The sweep needs running frames, so start the sensor if it is
             * parked - and park it again afterwards, leaving the bench as it
             * was found. One already running is left running. */
            if (!s->running) {
                s->probe_stop = true;
                s->running    = true;
            } else {
                s->probe_stop = false;
            }
            CLI_PRINTF_N("lux %s: sweeping SEL_PD on channel %d (%s), ~%u s%s",
                         s->label, (int)ch, CH_NAME[ch],
                         (PROBE_COUNT * 2u * (exposure_ms(s) + CONV_TIME_MS)) / 1000u,
                         s->probe_stop ? " (starting the sensor for it)" : "");
            CLI_PRINTF_N("     no samples while it runs%s",
                         s->probe_stop ? ", then it parks again" : "");
            return true;
        }

        if (is_bytes) {
            if (s->bytes_req) {
                CLI_PRINTF_N("lux %s: a byte dump is already queued", s->label);
                return true;
            }
            if (!s->up) {
                CLI_PRINTF_N("lux %s: no working I2C%u", s->label, s->bus);
                return true;
            }

            s->bytes_req = true;

            /* Needs running frames, like the sweep: start a parked sensor and
             * put it back afterwards, so the bench is left as it was found. */
            if (!s->running) {
                s->bytes_stop = true;
                s->running    = true;
            } else {
                s->bytes_stop = false;
            }
            CLI_PRINTF_N("lux %s: dumping raw channel bytes, %u frames, ~%u s%s",
                         s->label, BYTES_FRAMES,
                         (BYTES_FRAMES * (exposure_ms(s) + CONV_TIME_MS) + 999u) / 1000u,
                         s->bytes_stop ? " (starting the sensor for it)" : "");
            CLI_PRINTF_N("     no samples while it runs%s",
                         s->bytes_stop ? ", then it parks again" : "");
            return true;
        }

        if (is_dark) {
            /* ⚠️ The mixed acquisition owns SEL_PD: it switches channel 2 in and
             * out on its own cadence. A static mode set from here would either
             * be undone at the next refresh or, worse, hold channel 2 on the
             * covered diodes while every row still called it VIS. */
            if (s_sensor[0].meas_on || s_sensor[1].meas_on) {
                CLI_PRINTF_N("lux: the mixed acquisition drives channel 2 itself"
                             " - dark mode is not settable while it runs");
                CLI_PRINTF_N("     \"lux stop\" leaves the mode");
                return true;
            }

            /* Only sets the request: the measurement task applies it at a
             * frame boundary, so the CLI never writes SEL_PD while the task is
             * mid-transaction on that bus. Takes up to one exposure. */
            if (!*opt) {
                CLI_PRINTF_N("lux %s: channel 2 is on the %s photodiodes",
                             s->label, s->dark_mode ? "COVERED (dark)" : "visible");
                CLI_PRINTF_N("usage: lux dark <on|off>   (applies to both sensors)");
                return true;
            }
            bool on;
            if (ca_strncmp(opt, "on", 2) == 0)       on = true;
            else if (ca_strncmp(opt, "off", 3) == 0) on = false;
            else { CLI_PRINTF_N("usage: lux dark <on|off>   (both sensors)"); return true; }

            /* Both sensors, like the menu entry: the dark reference is only
             * worth reading if the two are configured the same way. The sensor
             * number stays in the syntax so the command reads the same as the
             * others, but it selects nothing - both are set. */
            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                sensor_ctx_t *t = &s_sensor[i];
                t->dark_req = on;
                if (t->running) continue;       /* its own task applies it */
                if (!vd_set_dark_mode(t, on))
                    CLI_PRINTF_N("lux %s: SEL_PD write failed - mode unchanged",
                                 t->label);
            }
            CLI_PRINTF_N("lux: dark mode %s on both sensors%s", on ? "on" : "off",
                         (s_sensor[0].running || s_sensor[1].running)
                             ? " (running ones at their next frame)" : "");
            if (on)
                CLI_PRINTF_N("     channel 2 becomes DARK - there is no vis reading while it is on");
            return true;
        }

        if (is_otp) {
            otp_report(s, (int)n);
            return true;
        }

        if (is_setup) {
            /* Configuring means writing to the part, and the measurement task
             * owns that bus while it runs. Same rule as "scan": ask for a stop
             * rather than reaching into a live transaction from another thread.
             * Refuse at the door, not per entry - a menu that opens and then
             * rejects every choice is worse than one that will not open.
             *
             * ⚠️ EVERY sensor must be stopped, not just the one named: the
             * settings are common now, so opening the menu on a parked sensor
             * would otherwise reconfigure a running one from the CLI thread. */
            for (u8 i = 0; i < SENSOR_COUNT; i++) {
                if (s_sensor[i].running) {
                    CLI_PRINTF_N("lux: stop every sensor first - %s is running"
                                 " (lux stop %u)",
                                 s_sensor[i].label, (u32)(i + 1u));
                    CLI_PRINTF_N("     setup is common to both, so both must be idle");
                    return true;
                }
            }
            if (*opt) {
                /* The menu is the interface now: a trailing word would be an
                 * old-syntax habit, and silently ignoring it would look like
                 * it had been applied. */
                CLI_PRINTF_N("lux setup takes no arguments - opening the menu"
                             " (it configures both sensors)");
            }
            s_menu_s = (u8)(n - 1);
            s_menu   = MENU_TOP;
            menu_show(s);
            return true;
        }

        if (is_start) { lux_start_one((u8)(n - 1)); return true; }

        /* is_stop */
        lux_stop_one((u8)(n - 1));
        return true;
    }

    if (ca_strncmp(p, "reg", 3) == 0) {
        /* Raw register access on sensor B, for finding what the datasheet does
         * not document. §4.1 says the registers "should be accessed only
         * through the VD6283TX software driver", which is precisely why a
         * bare-metal port needs a way to probe them by hand.
         *
         * Looking for the DARK bit: STALS.h documents the control as "dark
         * count will be output on channel 2", so the bit to find is the one
         * that makes CH2 (normally visible) stop following the light. Its
         * signature is unmistakable - cover the sensor and CH2 does NOT move,
         * because that photodiode is covered by construction.
         * Candidates: the RESERVED bits of CHANNEL6_ENABLE (0x2D) and of
         * ALS_CHANNEL_ENABLE (0x2E), both declared R/W. */
        const char *a = ca_skip_sp(p + 3);
        if (!*a) {
            CLI_PRINTF_N("usage: reg <addr> [value]   (hex, sensor B)");
            CLI_PRINTF_N("  reg 2D        read  CHANNEL6_ENABLE");
            CLI_PRINTF_N("  reg 2D 03     write bit0|bit1 and see if CH2 freezes");
            return true;
        }
        u16 idx = 0;
        u32 addr = (u32)ca_strtol_index(a, 16, &idx);
        const char *v = ca_skip_sp(a + idx);
        sensor_ctx_t *s = &s_sensor[1];        /* bus B: the one with a sensor */

        if (*v) {
            u32 val = (u32)ca_strtol(v, 16);
            if (vd_write_reg(s->bus, (u8)addr, (u8)val))
                CLI_PRINTF_N("reg 0x%02X <- 0x%02X", addr, val);
            else
                CLI_PRINTF_N("reg 0x%02X: write failed", addr);
        } else {
            u8 val = 0;
            if (vd_read_regs(s->bus, (u8)addr, &val, 1u))
                CLI_PRINTF_N("reg 0x%02X = 0x%02X", addr, val);
            else
                CLI_PRINTF_N("reg 0x%02X: read failed", addr);
        }
        return true;
    }

    if (ca_strncmp(p, "scan", 4) == 0) {
        /* Probe every 7-bit address on both buses. This is the test that tells
         * a wiring fault from an address mismatch: if nothing answers anywhere,
         * the problem is the bus (power, pull-ups, wires); if the part answers
         * at an address other than 0x20, the address is the problem. */
        for (u8 i = 0; i < SENSOR_COUNT; i++) {
            const sensor_ctx_t *s = &s_sensor[i];
            if (!s->up) {
                CLI_PRINTF_N("bus %s (I2C%u): not initialised", s->label, s->bus);
                continue;
            }
            if (s->running) {
                CLI_PRINTF_N("bus %s (I2C%u): stop it first (lux stop %u)",
                             s->label, s->bus, (u32)(i + 1u));
                continue;
            }
            CLI_PRINTF_N("bus %s (I2C%u): scanning...", s->label, s->bus);
            u32 found = 0;
            for (u8 a = 0x08u; a < 0x78u; a++) {
                u8 reg = VD6283_DEVICE_ID, val = 0;
                if (ca_i2c_write_read(s->bus, a, &reg, 1u, &val, 1u)) {
                    found++;
                    CLI_PRINTF_N("  0x%02X answers (reg0x00=0x%02X)%s", a, val,
                                 a == VD6283_ADDR ? "  <- expected" : "");
                }
            }
            if (found == 0)
                CLI_PRINTF_N("  nothing answered: check 3V3, GND, the two wires"
                             " and the pull-ups");
        }
        return true;
    }

    if (ca_strncmp(p, "map", 3) == 0) {
        print_wiring_map();
        return true;
    }

    if (ca_strncmp(p, "raw", 3) == 0) {
        for (u8 i = 0; i < SENSOR_COUNT; i++) {
            /* ⚠️ Zero-initialised, not just partly filled. vd_read_channels()
             * writes the counts and the settings, and the assignments below
             * cover the bookkeeping - but the row also reads dark_mode,
             * vis_calc, cal_k, settling and now meas, and those were left as
             * whatever the CLI stack happened to hold. A garbage dark_mode puts
             * the measured value under the wrong column name; a garbage `meas`
             * would send the row to the mixed-acquisition table outright.
             * The explicit assignments stay: they say what the values MEAN
             * here, which zero alone does not. */
            lux_sample_t one = { 0 };
            if (!s_sensor[i].present) {
                CLI_PRINTF_N("sensor %s: not present", s_sensor[i].label);
                continue;
            }
            if (vd_read_channels(&s_sensor[i], &one)) {
                one.seq      = 0;
                one.sensor   = i;
                one.bus      = s_sensor[i].bus;
                one.dropped     = s_sensor[i].dropped;
                one.adjusted    = 0;
                one.calibrating = 0;
                /* "raw" is a single immediate read, outside any run: it has no
                 * held dark and no cadence, so it always prints the full table
                 * even while the mixed acquisition is armed. */
                one.meas        = 0;
                one.dark_mode   = s_sensor[i].dark_mode ? 1u : 0u;
                print_sample(&one);
            } else {
                CLI_PRINTF_N("sensor %s: read failed", s_sensor[i].label);
            }
        }
        return true;
    }

#ifdef WIRE_A_PORT
    if (ca_strncmp(p, "wire", 4) == 0) {
        CLI_PRINTF_N("Continuity test - the sensors must be disconnected:");
        wire_test(WIRE_A_PORT, WIRE_A_SCL_PIN, WIRE_A_SCL);
        wire_test(WIRE_A_PORT, WIRE_A_SDA_PIN, WIRE_A_SDA);
        wire_test(WIRE_B_PORT, WIRE_B_SCL_PIN, WIRE_B_SCL);
        wire_test(WIRE_B_PORT, WIRE_B_SDA_PIN, WIRE_B_SDA);
        CLI_PRINTF_N("  reboot to hand the pins back to the I2C peripheral");
        return true;
    }
#endif

    if (ca_strncmp(p, "help", 4) == 0) {
        print_commands();
        return false;   /* let the built-in help print its own commands too */
    }

    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ============================================================================
 * ENTRY POINTS
 * ========================================================================== */

void ca_os_configure(void)
{
    ca_queue_static_init(&s_queue.q, sizeof(lux_sample_t), Q_DEPTH);

    EVAL_LED( ca_led_init(); )

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
    /* Standard mode: the sensor supports up to 1 MHz (§3.4.3), but 100 kHz is
     * the right default for jumper wires with discrete pull-ups. Sample rate
     * is set by the 1.6 s exposure, not by the bus. */
    for (u8 i = 0; i < SENSOR_COUNT; i++) {
        s_sensor[i].up = ca_i2c_init(&(ca_i2c_config_t){
            .i2c_num   = s_sensor[i].bus,
            .speed     = CA_I2C_SPEED_STANDARD,
            .alternate = s_sensor[i].alt });
    }
}

void ca_os_main(void)
{
    /* Two producers and one consumer. The measurement tasks are ordinary jobs:
     * they poll their own run flag as their first action, so they stay idle
     * until "lux start <n>" and each sensor can be brought up on
     * its own. With both free-running from reset there would be no way to tell
     * which bus a failure belongs to. The printer starts right away — it must
     * be draining the queue before the first sample arrives. */
    /* The 3rd argument of ca_os_job is what the task receives as its SECOND
     * parameter (thid comes first) - so 0 is sensor A and 1 is sensor B. */
    ca_os_job("lux_a", CA_TH_LEVEL3, task_measure, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("lux_b", CA_TH_LEVEL3, task_measure, (void *)(uintptr_t)1, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("lux_pr", CA_TH_LEVEL4, task_print, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}

#else  /* EN_I2C == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(mluxa, "=== lux_sensor : vd6283tx ===");
PRIVATE_STRING(mluxb, "EN_I2C == 0, skipping the VD6283TX application");
void ca_os_configure(void)
{
    PUT_N(&priv_mluxa);
    PUT_N(&priv_mluxb);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_I2C */
