// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    dac.h
 * @brief   DAC driver public API
 * @note    Implementation: hw/<family>/drivers/dac_<family>.c (bare-metal CMSIS)
 *          Fixed pins (analog, no AF):
 *          DAC CH1: PA4
 *          DAC CH2: PA5
 *          F103C8T6 (BluePill, medium-density): NO DAC — stub returns false.
 ******************************************************************************
 */

#ifndef DAC_H
#define DAC_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_DAC

typedef struct {
    u8 channel;   /* 1 = CH1 (PA4), 2 = CH2 (PA5) */
} ca_dac_config_t;

/* Initializes the specified DAC channel */
bool ca_dac_init(const ca_dac_config_t *cfg);

/* Writes a 12-bit right-aligned value (0–4095) to the channel */
void ca_dac_write(u8 channel, u16 val);

/* Disables the channel: the output buffer is released from the pin, which stays
 * in analog mode. Needed before driving that pin as a GPIO — an enabled DAC
 * keeps its buffer connected and fights whatever else drives the line. */
bool ca_dac_stop(u8 channel);

/* Comparator with a DAC channel as its threshold (U575, H753; elsewhere
 * ca_dac_comp_init() returns false). COMP1: input + on PB2 (COMP1_INP2,
 * CN9-13 on both Nucleo), input - the DAC channel inside the chip, so the
 * channel must be initialized and written first. The level is the plain
 * comparison, with the hysteresis the comparator adds around the threshold. */
typedef enum {
    CA_DAC_COMP_HYST_NONE = 0,
    CA_DAC_COMP_HYST_LOW,
    CA_DAC_COMP_HYST_MEDIUM,
    CA_DAC_COMP_HYST_HIGH
} ca_dac_comp_hyst_t;

bool ca_dac_comp_init(u8 dac_channel, ca_dac_comp_hyst_t hyst);
bool ca_dac_comp_level(void);       /* true = PB2 above the DAC threshold */
void ca_dac_comp_stop(void);

#endif /* EN_DAC */
#endif /* DAC_H */
