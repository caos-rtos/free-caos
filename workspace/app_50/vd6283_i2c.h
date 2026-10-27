// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * vd6283_i2c.h — the VD6283TX bus interface.
 *
 * ⚠️ Every function here talks to the part. The measurement task owns its
 * sensor's bus while it runs, so the CLI must only call these on a PARKED
 * sensor - otherwise a command cuts into a transaction mid-transfer.
 */

#ifndef VD6283_I2C_H
#define VD6283_I2C_H

#include "vd6283_ctrl.h"

#if EN_I2C

/* Raw register access. `bus` is the I2C instance, not a sensor index. */
bool vd_write_reg(u8 bus, u8 reg, u8 val);
bool vd_read_regs(u8 bus, u8 reg, u8 *dst, u16 len);

/* OTP: load the two banks, then decode them into s->otp. */
bool otp_load(sensor_ctx_t *s, u64 bits[2]);
void vd_read_otp(sensor_ctx_t *s);

/* Frame handshake: acknowledge the current frame and wait for the next. */
bool vd_next_frame(sensor_ctx_t *s);

/* Bring one sensor up in its configured mode. False if it does not answer or
 * does not identify itself; s->fail says which. */
bool vd_configure(sensor_ctx_t *s);

/* One coherent read of all six channels. */
bool vd_read_channels(sensor_ctx_t *s, lux_sample_t *out);

/* Move channel 2 between the visible and the covered photodiodes. */
bool vd_set_dark_mode(sensor_ctx_t *s, bool on);

/* Push the current gain and exposure. Stops the ALS engine around the writes -
 * ST refuses both operations on a running acquisition. */
bool ag_apply(sensor_ctx_t *s);

/* Measure the dark reference once and put channel 2 back on the visible
 * photodiodes. Costs three exposure windows; the count is RAW. */
bool vd_measure_dark(sensor_ctx_t *s, u32 *out);

/* How many values the sweep walks; the CLI uses it to state the duration. */
#define PROBE_COUNT     12u

/* How many frames the raw-byte dump takes. */
#define BYTES_FRAMES    6u

/* Diagnostic: print the FOUR bytes of every channel group separately, over
 * BYTES_FRAMES frames, with both assemblies side by side.
 *
 * It settled the question this port was built on the wrong answer to: the count
 * is at 0x06/0x07/0x08 (H/M/L, par. 4.3.6) and 0x05 + 4*ch is padding, so
 * reading the first three bytes of the group built H:M - the 16-bit format -
 * and never looked at L. Measured 2026-08-31 on both sensors: pad zero on all
 * 72 reads, low byte non-zero on all 72.
 *
 * Kept as a standing check rather than deleted: it is the only thing that reads
 * the padding byte, so it is what would catch the base address drifting back. */
void vd_dump_bytes(sensor_ctx_t *s);

/* Diagnostic: walk a set of SEL_PD values on one channel and print the counts.
 * Takes PROBE_COUNT x 2 frames, during which no samples are produced. */
void vd_probe_sweep(sensor_ctx_t *s, u8 ch);

#endif /* EN_I2C */
#endif /* VD6283_I2C_H */
