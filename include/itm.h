// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : itm.h
 * @brief          : ITM (Instrumentation Trace Macrocell) functions
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * ITM (Instrumentation Trace Macrocell) debug system for the caOS operating system.
 * Provides printf-like debug functionality through the SWO pin without
 * stopping CPU execution. Requires the SWO pin to be connected to the debugger.
 *
 * Typical sequence:
 * 1) ITM initialization -> send debug data -> display on debugger
 ******************************************************************************
 */

#ifndef ITM_H_
#define ITM_H_

#include "common.h"

/* =============================================================================
 * PUBLIC FUNCTIONS
 * ============================================================================= */

#if ENABLE_ITM

/* SWO baud rate. MUST stay at or below what the debug adapter can sample:
 * the ST-Link SWO capture runs at 1.8 MHz here, so a 2 MHz SWO transmits
 * FASTER than the adapter reads -> mis-sampled bits -> the adapter drops
 * packets -> it stops draining the trace -> the on-chip ITM FIFO overflows
 * (0x70 packets) and whole lines are lost, especially under the burst rate of
 * an -O3 Release build. 1 MHz is well below 1.8 MHz for comfortable sampling
 * and divides SystemCoreClock (168 MHz) exactly (ACPR = 167), so there is no
 * baud rounding error. Lower baud = lower bandwidth, but combined with the
 * FIFO-drain wait in ca_itm_send_byte the trace is loss-free. */
#define ITM_BAUDRATE    1000000UL

/**
 * @brief Initialize ITM for debugging output
 * @param sysclk System clock frequency in Hz (typically 168000000 for F407)
 * @param baudrate SWO baudrate (typically 2000000)
 * @note Call this before using ITM_SendChar or printf
 */
void ca_itm_init(u32 baudrate);

u16 ca_itm_printf( const char *format, ...);

u16 ca_itm_put(p_str string);

#else

#define ca_itm_init(RATE)

#define ca_itm_printf(MSG, ...)

#define ca_itm_put(MSG) 

#endif /* ENABLE_ITM */

#endif /* ITM_H_ */
