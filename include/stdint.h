// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#ifndef CUSTOM_STD_INT_H
#define CUSTOM_STD_INT_H

#include "def_mcu.h"

/* Signed types */
typedef i8      int8_t;
typedef i16     int16_t;
typedef i32     int32_t;
typedef i64     int64_t;

/* Unsigned types */
typedef u8      uint8_t;
typedef u16     uint16_t;
typedef u32     uint32_t;
typedef u64     uint64_t;

/* Pointer types (ARM 32-bit) */
typedef i32     intptr_t;
typedef u32     uintptr_t;

/* Maximum types (long long, 64-bit) */
typedef i64     intmax_t;
typedef u64     uintmax_t;

/* Least types (guaranteed minimum) */
typedef i8      int_least8_t;
typedef u8      uint_least8_t;
typedef i16     int_least16_t;
typedef u16     uint_least16_t;
typedef i32     int_least32_t;
typedef u32     uint_least32_t;
typedef i64     int_least64_t;
typedef u64     uint_least64_t;

/* Fast types */
typedef i32     int_fast8_t;
typedef u32     uint_fast8_t;
typedef i32     int_fast16_t;
typedef u32     uint_fast16_t;
typedef i32     int_fast32_t;
typedef u32     uint_fast32_t;
typedef i64     int_fast64_t;
typedef u64     uint_fast64_t;

/* Limits */
#define INT8_MIN     (-128)
#define INT8_MAX     (127)
#define UINT8_MAX    (255U)

#define INT16_MIN    (-32768)
#define INT16_MAX    (32767)
#define UINT16_MAX   (65535U)

#define INT32_MIN    (-2147483647 - 1)
#define INT32_MAX    (2147483647)
#define UINT32_MAX   (4294967295U)

#define INT64_MIN    (-9223372036854775807LL - 1)
#define INT64_MAX    (9223372036854775807LL)
#define UINT64_MAX   (18446744073709551615ULL)

#define INTMAX_MIN   INT64_MIN
#define INTMAX_MAX   INT64_MAX
#define UINTMAX_MAX  UINT64_MAX

#define INTPTR_MIN   INT32_MIN
#define INTPTR_MAX   INT32_MAX
#define UINTPTR_MAX  UINT32_MAX

#define SIZE_MAX     UINT32_MAX

/* Constant macros */
#define INT8_C(x)    (x)
#define UINT8_C(x)   (x##U)
#define INT16_C(x)   (x)
#define UINT16_C(x)  (x##U)
#define INT32_C(x)   (x)
#define UINT32_C(x)  (x##U)
#define INT64_C(x)   (x##LL)
#define UINT64_C(x)  (x##ULL)
#define INTMAX_C(x)  (x##LL)
#define UINTMAX_C(x) (x##ULL)

#endif /*CUSTOM_STD_INT_H*/