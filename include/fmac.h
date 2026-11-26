// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    fmac.h
 * @brief   Filter math accelerator: FIR and IIR filters in q1.15
 ******************************************************************************
 * Paid edition only (EN_FMAC).
 */
#ifndef FMAC_H
#define FMAC_H

#include "common.h"
#include "os_config.h"

#if EN_FMAC

/*
 * Present on H5 and U5 only; elsewhere ca_fmac_init() returns false and every
 * call fails. Paid edition only (EN_FMAC).
 *
 * The unit computes, for each input sample (RM0456 26.3.6, RM0481 21.3.6):
 *
 *     y[n] = 2^gain * ( sum b[k] x[n-k]  +  sum a[k] y[n-k] )
 *                       k=0..nb-1           k=1..na
 *
 * Note the PLUS on the feedback: a[] is minus the denominator of the usual
 * convention (scipy lfilter a[1..]), already scaled into q1.15. A filter whose
 * coefficients exceed 1 is scaled down by 2^-gain and gain gives it back.
 *
 *   FIR  nb 2..127, na 0          IIR  nb 2..64, na 1..nb-1 (direct form 1)
 *
 * Inputs, outputs and coefficients are q1.15; the accumulator is q4.22 and
 * wraps silently (CA_FMAC_SAT tells it happened). With clip the output
 * saturates at +-1 instead of wrapping. Use clip on an IIR: a wrapped output
 * goes back through the feedback and the filter can oscillate at full scale
 * for good (ex102: hp100, 0.9 FS at 400..700 Hz). Clip is not enough near
 * full scale: an IIR can settle clipping every period (ex102, hp100 at 500 Hz,
 * 1 run in 5 on the U575). Feed an IIR at least 6 dB below full scale.
 *
 * ca_fmac_start() clears the delay lines, so the first output already sees
 * x[-1..] = y[-1..] = 0 (as lfilter does). The state then stays in the unit:
 * successive ca_fmac_run() calls are one continuous stream, one output per
 * input. One filter at a time, used by one task; polling, task context only.
 */

#define CA_FMAC_FIR_MAX   127U       /* feed-forward taps of a FIR */
#define CA_FMAC_IIR_MAX   64U        /* feed-forward taps of an IIR */

/* Error flags since ca_fmac_start() (ca_fmac_flags) */
#define CA_FMAC_SAT       0x01U      /* accumulator wrapped */
#define CA_FMAC_OVFL      0x02U      /* input written with the X1 buffer full */
#define CA_FMAC_UNFL      0x04U      /* output read with the Y buffer empty */

typedef struct {
    const i16 *b;      /* feed-forward coefficients b[0..nb-1] */
    u32        nb;
    const i16 *a;      /* feedback a[1..na] (a[0] is not passed), NULL for a FIR */
    u32        na;
    u32        gain;   /* output x 2^gain, 0..7 */
    bool       clip;   /* saturate the output instead of wrapping */
} ca_fmac_filter_t;

bool ca_fmac_init(void);                              /* false: no FMAC on this part */
bool ca_fmac_start(const ca_fmac_filter_t *f);        /* loads the coefficients, zero state, runs */
u32  ca_fmac_run(const i16 *in, i16 *out, u32 len);   /* samples filtered; < len only on a stall */
void ca_fmac_stop(void);
u32  ca_fmac_flags(void);                             /* CA_FMAC_* */

#endif /* EN_FMAC */

#endif /* FMAC_H */
