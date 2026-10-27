// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    adc.h
 * @brief   ADC driver public API
 * @note    Implementation: hw/<family>/drivers/adc_<family>.c (bare-metal CMSIS)
 *          Single-shot polling, 12-bit right-aligned.
 *
 *          Analog pins per channel (common to all families):
 *          CH0=PA0, CH1=PA1, CH2=PA2, CH3=PA3, CH4=PA4, CH5=PA5
 *          CH6=PA6, CH7=PA7, CH8=PB0,  CH9=PB1
 *          CH10=PC0, CH11=PC1, CH12=PC2, CH13=PC3, CH14=PC4, CH15=PC5
 *          CH16..18 = internal (temp, vref, vbat) — no GPIO
 *
 *          TODO: DMA + timer trigger for continuous acquisition
 ******************************************************************************
 */

#ifndef ADC_H
#define ADC_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_ADC

typedef struct {
    u8 adc_num;      /* 1, 2, 3 */
    u8 sample_time;  /* 0..7 (SMPx bits, family-specific encoding) */
} ca_adc_config_t;

/* Initializes the ADC (clock + calibration + enable) */
bool ca_adc_init(const ca_adc_config_t *cfg);

/* Configures the GPIO pin as analog for the indicated channel.
 * Call before ca_adc_read for external channels (CH0..CH15).
 * Not necessary for internal channels (CH16..CH18). */
void ca_adc_pin_init(u8 adc_num, u8 channel);

/* Converts a channel — single-shot polling, returns 12-bit value (0..4095) */
u16 ca_adc_read(u8 adc_num, u8 channel);

/* ISR callback: called by the IRQ handler with the converted value */
typedef void (*ca_adc_isr_cb_t)(u8 adc_num, u16 value);

/* Initializes the ADC in ISR mode (enables EOCIE + NVIC) */
bool ca_adc_init_isr(const ca_adc_config_t *cfg, ca_adc_isr_cb_t cb);

/* Disables the ADC: any conversion in flight is stopped and ADEN is cleared,
 * which also releases the analog switch inside the I/O pad. Needed before
 * driving an ADC input as a GPIO. */
bool ca_adc_stop(u8 adc_num);

/* Interrupt-path handshake. A polled ca_adc_read() and an ISR conversion on the
 * same ADC would steal each other's result, so an ISR run marks the ADC busy
 * and ca_adc_read() returns 0 until it ends. Ownership is a flag, not a mutex:
 * the run spans several calls and outlives the task that started it, and a
 * mutex held across that would deadlock the moment that same task read the ADC.
 * ca_adc_isr_done() releases it and is safe to call from an ISR;
 * "adc stop <n>" and ca_adc_stop() call it for you. */
void ca_adc_isr_done(u8 adc_num);

/* Starts a single conversion in ISR mode (non-blocking) */
void ca_adc_start_isr(u8 adc_num, u8 channel);

/* ADC IRQ handler — called by handlers.c (ADC_IRQHandler / ADC3_IRQHandler) */
void ca_adc_irq_handler(u8 adc_num);

#else  /* EN_ADC == 0 */

/* ADC disabled: keep only the ISR handler declared (trap stub in adc.c) so the
 * weak ADC_IRQHandler in handlers.c still compiles. Mirrors the spi.h/i2c.h
 * #else convention. */
void ca_adc_irq_handler(u8 adc_num);

#endif /* EN_ADC */
#endif /* ADC_H */
