// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : temp_sensor.h
 * @brief          : Internal temperature sensor driver interface for CAOS
 * @author         : Angelo Coppi
 ******************************************************************************
 */

#ifndef TEMP_SENSOR_H
#define TEMP_SENSOR_H


#include "caos.h"

/* DTS (digital temperature sensor, H563 only): shared with hw/ */
#define CA_TEMP_NO_DTS   (-99900)                /* centi-degC: no DTS on this part */
typedef void (*ca_temp_alarm_cb_t)(bool hot);    /* called from the DTS interrupt */

#if ENABLE_SENSOR

/* =============================================================================
 * PUBLIC API
 * ============================================================================= */

bool ca_temp_init(void);
i32  ca_temp_read(void);         /* temperature in integer °C, VREFINT compensated */
u16  ca_temp_read_raw(void);     /* raw ADC value (12-bit) */

/* DTS: independent of the ADC and of VREF+. False / CA_TEMP_NO_DTS elsewhere. */
bool ca_temp_dts_present(void);
i32  ca_temp_read_dts(void);     /* centi-degC, averaged */
bool ca_temp_set_alarm(i32 cold_c, i32 hot_c, ca_temp_alarm_cb_t cb);
void ca_temp_retune(void);       /* after a core clock change (see ca_pwr_set_freq) */
#else
#define ca_temp_init() 
#define ca_temp_read()  
#define ca_temp_read_raw()
#define ca_temp_retune()
#endif

#endif /* TEMP_SENSOR_H */
