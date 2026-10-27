// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : endian.h
 * @brief          : Functions for endian conversion (byte swap)
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Header for endian conversion functions for u16, u32, u64
 * and i16, i32, i64 types. Uses Cortex-M inline assembler instructions
 * or GCC compiler builtins for maximum performance.
 *
 * Available functions:
 * - Big-endian <-> little-endian conversion
 * - Host <-> network byte order conversion
 * - Support for signed and unsigned types
 ******************************************************************************
 */

#ifndef CAOS_ENDIAN_H
#define CAOS_ENDIAN_H

#include "os_config.h"



#include "common.h"

/* ============================================================================
 * MACROS AND DEFINITIONS
 * ========================================================================== */

/* Platform endianness detection */
#if defined(__BYTE_ORDER__)
    #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        #define CAOS_ENDIAN_LITTLE  1
        #define CAOS_ENDIAN_BIG     0
    #elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        #define CAOS_ENDIAN_LITTLE  0
        #define CAOS_ENDIAN_BIG     1
    #endif
#else
    /* Fallback for ARM Cortex-M (always little-endian) */
    #define CAOS_ENDIAN_LITTLE  1
    #define CAOS_ENDIAN_BIG     0
#endif

/* ============================================================================
 * BYTE SWAP FUNCTIONS - IMPLEMENTATION
 * ========================================================================== */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Byte swap for u16 (swaps 2 bytes)
 * @param value Value to convert
 * @return Value with swapped bytes
 */
static inline u16 ca_bswap16(u16 value) {
#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) || \
    defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_BASE__) || \
    defined(__ARM_ARCH_8M_MAIN__)
    /* Use REV16 instruction for Cortex-M */
    u16 result;
    __asm volatile ("rev16 %0, %1" : "=r" (result) : "r" (value));
    return result;
#elif defined(__GNUC__)
    /* Use GCC builtin */
    return (u16)__builtin_bswap16(value);
#else
    /* Portable fallback */
    return (u16)((value >> 8) | (value << 8));
#endif
}

/**
 * @brief Byte swap for u32 (swaps 4 bytes)
 * @param value Value to convert
 * @return Value with swapped bytes
 */
static inline u32 ca_bswap32(u32 value) {
#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) || \
    defined(__ARM_ARCH_7EM__) || defined(__ARM_ARCH_8M_BASE__) || \
    defined(__ARM_ARCH_8M_MAIN__)
    /* Use REV instruction for Cortex-M */
    u32 result;
    __asm volatile ("rev %0, %1" : "=r" (result) : "r" (value));
    return result;
#elif defined(__GNUC__)
    /* Use GCC builtin */
    return __builtin_bswap32(value);
#else
    /* Portable fallback */
    return ((value >> 24) & 0x000000FFU) |
           ((value >>  8) & 0x0000FF00U) |
           ((value <<  8) & 0x00FF0000U) |
           ((value << 24) & 0xFF000000U);
#endif
}

/**
 * @brief Byte swap for u64 (swaps 8 bytes)
 * @param value Value to convert
 * @return Value with swapped bytes
 */
static inline u64 ca_bswap64(u64 value) {
#if defined(__GNUC__)
    /* Use GCC builtin if available */
    #if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
    return __builtin_bswap64(value);
    #else
    /* Manual implementation for old GCC */
    u32 lo = (u32)(value >> 32);
    u32 hi = (u32)value;
    return (((u64)ca_bswap32(hi)) << 32) | (u64)ca_bswap32(lo);
    #endif
#else
    /* Manual implementation */
    u32 lo = (u32)(value >> 32);
    u32 hi = (u32)value;
    return (((u64)ca_bswap32(hi)) << 32) | (u64)ca_bswap32(lo);
#endif
}

/**
 * @brief Byte swap for i16 (swaps 2 bytes)
 * @param value Value to convert
 * @return Value with swapped bytes
 */
static inline i16 ca_bswap16_signed(i16 value) {
    return (i16)ca_bswap16((u16)value);
}

/**
 * @brief Byte swap for i32 (swaps 4 bytes)
 * @param value Value to convert
 * @return Value with swapped bytes
 */
static inline i32 ca_bswap32_signed(i32 value) {
    return (i32)ca_bswap32((u32)value);
}

/**
 * @brief Byte swap for i64 (swaps 8 bytes)
 * @param value Value to convert
 * @return Value with swapped bytes
 */
static inline i64 ca_bswap64_signed(i64 value) {
    return (i64)ca_bswap64((u64)value);
}

/* ============================================================================
 * HOST <-> NETWORK BYTE ORDER CONVERSIONS
 * Network byte order is big-endian
 * ========================================================================== */

#if CAOS_ENDIAN_LITTLE

/* Host is little-endian, so conversion is needed for network order (big-endian) */

#define ca_htons(x)     ca_bswap16(x)   /* host to network short (16-bit) */
#define ca_ntohs(x)     ca_bswap16(x)   /* network to host short (16-bit) */
#define ca_htonl(x)     ca_bswap32(x)   /* host to network long (32-bit) */
#define ca_ntohl(x)     ca_bswap32(x)   /* network to host long (32-bit) */
#define ca_htonll(x)    ca_bswap64(x)   /* host to network long long (64-bit) */
#define ca_ntohll(x)    ca_bswap64(x)   /* network to host long long (64-bit) */

/* Signed versions */
#define ca_htons_signed(x)   ca_bswap16_signed(x)
#define ca_ntohs_signed(x)   ca_bswap16_signed(x)
#define ca_htonl_signed(x)   ca_bswap32_signed(x)
#define ca_ntohl_signed(x)   ca_bswap32_signed(x)
#define ca_htonll_signed(x)  ca_bswap64_signed(x)
#define ca_ntohll_signed(x)  ca_bswap64_signed(x)

#else

/* Host is big-endian, network order is equal to host order */

#define ca_htons(x)     (x)
#define ca_ntohs(x)     (x)
#define ca_htonl(x)     (x)
#define ca_ntohl(x)     (x)
#define ca_htonll(x)    (x)
#define ca_ntohll(x)    (x)

/* Signed versions */
#define ca_htons_signed(x)   (x)
#define ca_ntohs_signed(x)   (x)
#define ca_htonl_signed(x)   (x)
#define ca_ntohl_signed(x)   (x)
#define ca_htonll_signed(x)  (x)
#define ca_ntohll_signed(x)  (x)

#endif

/* ============================================================================
 * HOST <-> LITTLE ENDIAN CONVERSIONS
 * ========================================================================== */

#if CAOS_ENDIAN_LITTLE

/* Host is already little-endian */
#define ca_htole16(x)   (x)
#define ca_le16toh(x)   (x)
#define ca_htole32(x)   (x)
#define ca_le32toh(x)   (x)
#define ca_htole64(x)   (x)
#define ca_le64toh(x)   (x)

/* Host to big-endian */
#define ca_htobe16(x)   ca_bswap16(x)
#define ca_be16toh(x)   ca_bswap16(x)
#define ca_htobe32(x)   ca_bswap32(x)
#define ca_be32toh(x)   ca_bswap32(x)
#define ca_htobe64(x)   ca_bswap64(x)
#define ca_be64toh(x)   ca_bswap64(x)

#else

/* Host is big-endian */
#define ca_htole16(x)   ca_bswap16(x)
#define ca_le16toh(x)   ca_bswap16(x)
#define ca_htole32(x)   ca_bswap32(x)
#define ca_le32toh(x)   ca_bswap32(x)
#define ca_htole64(x)   ca_bswap64(x)
#define ca_le64toh(x)   ca_bswap64(x)

#define ca_htobe16(x)   (x)
#define ca_be16toh(x)   (x)
#define ca_htobe32(x)   (x)
#define ca_be32toh(x)   (x)
#define ca_htobe64(x)   (x)
#define ca_be64toh(x)   (x)

#endif


#ifdef __cplusplus
}
#endif



#endif /* CAOS_ENDIAN_H */
