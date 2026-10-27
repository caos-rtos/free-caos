// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : utils.h
 * @brief          : Utility functions for conversion and formatting
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Utility functions for the caOS operating system.
 * Includes functionality for printf-style string formatting,
 * numeric conversions, and other common operations useful in an embedded environment.
 * Provides lightweight and optimized implementations for microcontrollers without
 * full standard library support.
 *
 * Typical sequence:
 * 1) Include header -> use utility functions for formatting/conversion
 ******************************************************************************
 */

#ifndef UTILS_HEADER
#define UTILS_HEADER

#include "common.h"
#include <stdarg.h>

/* =============================================================================
 * MINI SNPRINTF - Minimal printf-style formatting
 * ============================================================================= */

/**
 * @brief  Minimal version of vsnprintf for embedded systems (with va_list)
 *
 * @details Supports the following format specifiers:
 *          - %d  : signed decimal (i32)
 *          - %u  : unsigned decimal (u32)
 *          - %x  : hexadecimal lowercase (u32, without zero-padding)
 *          - %X  : hexadecimal uppercase (u32, without zero-padding)
 *          - %b  : binary (u32, without zero-padding)
 *          - %c  : character (i8)
 *          - %s  : string (i8*)
 *          - %%  : literal '%'
 *
 * @param  buffer - Destination buffer
 * @param  size - Maximum buffer size (including '\0')
 * @param  format - Format string
 * @param  args - Variable argument list (va_list)
 *
 * @retval int - Number of characters written (excluding '\0'), or -1 on error
 *
 * @note   The function always guarantees null-termination if size > 0
 * @note   If size is insufficient, the string is truncated
 * @note   Does NOT support: width, precision, flags, length modifiers
 * @note   This is the "internal" version used by ca_snprintf
 */
int ca_vsnprintf( char  *buffer, u32 size, const char  *format, va_list args);

/**
 * @brief  Is this pointer inside a memory area this part actually has?
 *
 * Checked against the board's own map (CA_MEMMAP_TABLE in objmap.h, the same
 * table behind ca_memmap_get), so nothing is hard-coded: an address outside
 * the declared FLASH and RAM areas of THIS MCU is invalid by construction.
 *
 * Use it before dereferencing a pointer whose origin you do not control - a
 * varargs argument, a value read back from a device, a field in a structure
 * that may have been overwritten. NULL is only the easiest bad pointer to
 * catch; the ones that actually show up are small integers that drifted into
 * the wrong slot (0x1, a length, an enum) and fault exactly the same way.
 *
 * @param  ptr - pointer to test (NULL is invalid)
 * @retval true if the address falls inside a declared memory area
 * @note   Compile-time constant table, a couple of compares: ISR-safe, and it
 *         does not read the pointed-to memory.
 */
bool ca_valid_pointer(const void *ptr);

/**
 * @brief  Minimal version of snprintf for embedded systems
 *
 * @details Supports the following format specifiers:
 *          - %d  : signed decimal (i32)
 *          - %u  : unsigned decimal (u32)
 *          - %x  : hexadecimal lowercase (u32, without zero-padding)
 *          - %X  : hexadecimal uppercase (u32, without zero-padding)
 *          - %b  : binary (u32, without zero-padding)
 *          - %c  : character (i8)
 *          - %s  : string (i8*)
 *          - %%  : literal '%'
 *
 * @param  buffer - Destination buffer
 * @param  size - Maximum buffer size (including '\0')
 * @param  format - Format string
 * @param  ... - Variable arguments
 *
 * @retval int - Number of characters written (excluding '\0'), or -1 on error
 *
 * @note   The function always guarantees null-termination if size > 0
 * @note   If size is insufficient, the string is truncated
 * @note   Does NOT support: width, precision, flags, length modifiers
 *
 * @example
 *   i8 buf[64];
 *   ca_snprintf(buf, sizeof(buf), "Temp: %d C, Addr: 0x%X", temp, addr);
 *   // Output: "Temp: 25 C, Addr: 0x1234ABCD"
 *
 * @example
 *   i8 buf[32];
 *   ca_snprintf(buf, sizeof(buf), "Pin %u = %b", pin, state);
 *   // Output: "Pin 5 = 00000000000000000000000000000001"
 */
int ca_snprintf( char  *buffer, u32 size, const char  *format, ...);


/* =============================================================================
 * PUTTER PRINTF - Output via callback function
 * ============================================================================= */



/**
 * @brief Printf with output via sender
 * @param out Sender function for output
 * @param buf Buffer for formatting
 * @param size Buffer size
 * @param format Format string
 * @return Number of characters written
 */
u16 ca_printf(sender out, char *buf, u16 size, const char  *format, ...);
u16 ca_printf_n(sender out, char *buf, u16 size, const char  *format, ...);


/**
 * @brief Put with output via sender
 * @param out Sender function for output
 * @param msg string to send
 * @return Number of characters written
 */
u16 ca_put(sender out, c_str msg);
u16 ca_put_n(sender out, c_str msg);

/**
 * @brief Print CPU, stack, and memory statistics (top-style)
 * @note To be called periodically (e.g., every 5000 cycles)
 */
static inline void ca_top(u16 mode) {
    ca_scheduler_tasks_info((ca_scheduler_tasks_info_mode)(mode));
}

/**
 * @brief Converts a string to a 32-bit integer
 * @param buff Pointer to the string to convert
 * @param base Numeric base (e.g., 10 for decimal, 16 for hexadecimal)
 * @return Converted integer value
 */

i32 ca_strtol_index(const char *buff, u16 base,u16 *index);

static inline i32 ca_strtol(const char *buff, u16 base){
    return ca_strtol_index(buff, base,NULL);
}

static inline const char *ca_skip_sp(const char *s)
{
    while (*s == ' ') s++;
    return s;
}

/* NOTE: the second argument is a BYTE COUNT, not a line count - it used to
 * be named "lines" and every caller already passed bytes (cli.c passes
 * lines*16). The name was the trap: it made ca_memory_dump pass a line count
 * straight through and dump 1/16 of what was asked. */
void ca_hexdump( u8 *ptr,u16 size);

void ca_hexdump_u32( u32 *ptr,u16 size);


#endif