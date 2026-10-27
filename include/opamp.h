// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    opamp.h
 * @brief   Operational amplifiers: follower or internal PGA (x2..x16)
 ******************************************************************************
 * U575 and H753 have two; elsewhere ca_opamp_count() is 0 and init fails.
 * The non-inverting input is its pin or the DAC channel of the same number
 * (OPAMP1 <- DAC CH1, OPAMP2 <- DAC CH2): with the DAC the whole chain stays
 * inside the chip. The output is always on its pin, and on an ADC channel
 * where the part routes it (ca_opamp_adc_channel).
 *
 *   OPAMP  U575 out / ADC     H753 out / ADC
 *   1      PA3  / ADC1 ch8    PC4  / ADC1 ch4 (Nucleo: RMII_RXD0 via SB36)
 *   2      PB0  / ADC1 ch15   PE7  / none
 *
 * Factory offset trim (U575: within +-7 mV). PGA gain error +-1% (DS13737).
 * Keep the input below VDDA / gain, or the output saturates.
 * No EN_ flag: unused code is dropped by the linker.
 */
#ifndef OPAMP_H
#define OPAMP_H

#include "caos.h"

typedef enum {
    CA_OPAMP_IN_PIN = 0,     /* OPAMPx_VINP pin */
    CA_OPAMP_IN_DAC          /* DAC channel x, internal */
} ca_opamp_input_t;

u32  ca_opamp_count(void);                                       /* 0: none on this part */
bool ca_opamp_init(u32 num, ca_opamp_input_t in, u32 gain);      /* gain 1 = follower, 2/4/8/16 = PGA */
void ca_opamp_stop(u32 num);
u8   ca_opamp_adc_channel(u32 num, u8 *adc_num);                 /* 0: output not routed to an ADC */

#endif /* OPAMP_H */
