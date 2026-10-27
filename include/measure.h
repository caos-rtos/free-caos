// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    measure.h
 * @brief   Time profiling via DWT CYCCNT — no HW timer occupied
 ******************************************************************************
 */

#ifndef MEASURE_H_
#define MEASURE_H_

#include "common.h"


void ca_measure_start(u32 *p_start);
void ca_measure_stop(volatile u32 *p);

#endif /* MEASURE_H_ */
