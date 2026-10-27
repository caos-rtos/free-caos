// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    rng.h
 * @brief   RNG driver public API — random number generator
 * @note    TRNG hardware on: F4xx (except F446), F7xx, H5xx, H7xx, U5xx.
 *          F103 and F446 have no TRNG (MCU_HAS_RNG 0) and rng.c compiles a
 *          software CSPRNG instead, so the API below behaves the same on
 *          every part. EN_RNG says the API exists; MCU_HAS_RNG says whether
 *          the entropy is hardware — ask ca_rng_is_hardware() at runtime.
 ******************************************************************************
 */

#ifndef RNG_H
#define RNG_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_RNG

/* init: enable clock + configure HW RNG, or seed the software backend */
bool ca_rng_init(void);

/* single 32-bit random value (blocking, mutex protected) */
bool ca_rng_get(u32 *val);

/* fill buffer with random data */
bool ca_rng_fill(u8 *buf, u32 len);

/* true when the bytes come from a hardware TRNG rather than the software
 * fallback. Long-term key generation should refuse to run when this is false. */
bool ca_rng_is_hardware(void);

#endif /* EN_RNG */
#endif /* RNG_H */
