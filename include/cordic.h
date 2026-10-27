// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    cordic.h
 * @brief   CORDIC coprocessor: trigonometric/hyperbolic functions in q1.31
 ******************************************************************************
 * Present on H5 and U5 only; elsewhere ca_cordic_init() returns false and
 * every call fails. Arguments and results are q1.31 (-1 .. 1-2^-31), with the
 * conventions of the reference manual (RM0456 25.3.2, RM0481 20.3.2):
 *
 *   func      arg1           arg2       res1             res2       scale n
 *   COS/SIN   angle/pi       modulus m  m*cos | m*sin    the other  0
 *   PHASE     x              y          atan2(y,x)/pi    modulus    0
 *   MODULUS   x              y          modulus          phase/pi   0
 *   ATAN      x*2^-n         -          atan(x)/(pi*2^n) -          0..7
 *   COSH/SINH x*2^-1         -          cosh|sinh / 2    the other  1
 *   ATANH     x*2^-1         -          atanh(x)/2       -          1
 *   LN        x*2^-n         -          ln(x)/2^(n+1)    -          1..4
 *   SQRT      x*2^-n         -          sqrt(x)/2^n      -          0..2
 *
 * 24 iterations (q1.31 limit): error below 2^-19 on the circular functions.
 * Each call runs inside a critical section of a few tens of cycles, so the
 * coprocessor can be shared between tasks. Task context only.
 * No EN_ flag: unused code is dropped by the linker.
 */
#ifndef CORDIC_H
#define CORDIC_H

#include "caos.h"

typedef enum {
    CA_CORDIC_COS = 0,
    CA_CORDIC_SIN,
    CA_CORDIC_PHASE,
    CA_CORDIC_MODULUS,
    CA_CORDIC_ATAN,
    CA_CORDIC_COSH,
    CA_CORDIC_SINH,
    CA_CORDIC_ATANH,
    CA_CORDIC_LN,
    CA_CORDIC_SQRT
} ca_cordic_func_t;

bool ca_cordic_init(void);                       /* false: no CORDIC on this part */
bool ca_cordic_calc(ca_cordic_func_t func, u32 scale, i32 arg1, i32 arg2,
                    i32 *res1, i32 *res2);       /* res2 may be NULL; 0 when unused */
bool ca_cordic_sincos(i32 angle, i32 *sin_q31, i32 *cos_q31);   /* angle = rad/pi */
bool ca_cordic_polar(i32 x, i32 y, i32 *angle, i32 *modulus);   /* angle = rad/pi */

#endif /* CORDIC_H */
