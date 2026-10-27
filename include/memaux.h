// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : memaux.h
 * @brief          : Memory and String Auxiliary Functions
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Auxiliary functions for memory and string operations to replace
 * newlib library functions. Includes functions for copying, setting,
 * comparing data in memory and string operations such as comparison,
 * copy and length calculation.
 *
 * Typical sequence:
 * 1) Include header -> use memory/string utility functions
 ******************************************************************************
 */

#ifndef MEMAUX_H_
#define MEMAUX_H_

#include "common.h"

/* =============================================================================
 * MEMORY FUNCTIONS
 * ============================================================================= */

/**
 * @brief Copy memory (byte by byte)
 * @param dest Destination
 * @param src Source
 * @param count Number of bytes
 * @return Pointer to dest
 */
void* ca_memcpy(volatile void *dest, const void *src, u32 count);

/**
 * @brief Set memory to a value (byte by byte)
 * @param dest Destination
 * @param value Value to write
 * @param count Number of bytes
 * @return Pointer to dest
 */
void* ca_memset(volatile void *dest, u8 value, u32 count);

/**
 * @brief Zero out memory (byte by byte)
 * @param dest Destination
 * @param count Number of bytes
 * @return Pointer to dest
 */
void* ca_memzero(volatile void *dest, u32 count);

/**
 * @brief Compare memory (byte by byte)
 * @param ptr1 First buffer
 * @param ptr2 Second buffer
 * @param count Number of bytes
 * @return 0 if equal, <0 if ptr1 < ptr2, >0 if ptr1 > ptr2
 */
i32 ca_memcmp(const void *ptr1, const void *ptr2, u32 count);

/**
 * @brief Set u32 memory to a value — unroll ×4 + remainder
 * @param dest Destination (u32 array, 4B aligned)
 * @param value u32 value to write
 * @param count Number of u32 (not bytes!)
 * @return Pointer to dest
 */
void* ca_memset_u32(volatile u32 *dest, u32 value, u32 count);

/**
 * @brief Copy u32 memory — unroll ×4 + remainder
 * @param dest Destination (u32 array, 4B aligned)
 * @param src  Source (u32 array, 4B aligned)
 * @param count Number of u32 (not bytes!)
 * @return Pointer to dest
 */
void* ca_memcpy_u32(volatile u32 *dest, const u32 *src, u32 count);

/**
 * @brief Set u16 memory to a value — internally pack→u32, unroll ×4 + remainder
 * @param dest  Destination (u16 array, 2B aligned)
 * @param value u16 value to write
 * @param count Number of u16 (not bytes!)
 * @return Pointer to dest
 */
void* ca_memset_u16(volatile u16 *dest, u16 value, u32 count);

/**
 * @brief Copy u16 memory — internally cast→u32 (count/2), unroll ×4 + u16 remainder
 * @param dest  Destination (u16 array, 2B aligned)
 * @param src   Source (u16 array, 2B aligned)
 * @param count Number of u16 (not bytes!)
 * @return Pointer to dest
 */
void* ca_memcpy_u16(volatile u16 *dest, const u16 *src, u32 count);


/* =============================================================================
 * STRING FUNCTIONS
 * ============================================================================= */

/**
 * @brief Compare two strings (case sensitive)
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, <0 if s1 < s2, >0 if s1 > s2
 */
i32 ca_strcmp(const char *s1, const char *s2);

/**
 * @brief Compare two strings (case insensitive)
 * @param s1 First string
 * @param s2 Second string
 * @return 0 if equal, <0 if s1 < s2, >0 if s1 > s2
 */
i32 ca_stricmp(const char *s1, const char *s2);

/**
 * @brief Compare two strings with limit (case sensitive)
 * @param s1 First string
 * @param s2 Second string
 * @param max Maximum number of characters to compare
 * @return 0 if equal, <0 if s1 < s2, >0 if s1 > s2
 */
i32 ca_strncmp(const char *s1, const char *s2, u32 max);

/**
 * @brief Copy string
 * @param dest Destination
 * @param src Source
 * @return Pointer to dest
 */
char* ca_strcpy(char *dest, const char *src);

/**
 * @brief Copy string with limit
 * @param dest Destination
 * @param src Source
 * @param max Maximum number of characters (including '\0')
 * @return Pointer to dest
 * @note Always guarantees '\0' termination
 */
char* ca_strncpy(char *dest, const char *src, u32 max);

/**
 * @brief Calculate string length
 * @param str String
 * @return Length (excluding '\0')
 */
u32 ca_strlen(const char *str);


#endif /* MEMAUX_H_ */
