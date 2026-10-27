// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : wdt.h
 * @brief          : Watchdog Timer driver interface for CAOS
 * @author         : Angelo Coppi
 ******************************************************************************
 */

#ifndef WDT_H
#define WDT_H

#include "caos.h"

/* =============================================================================
 * WATCHDOG TYPE ENUMERATION
 * ============================================================================= */

typedef enum {
    CA_WDT_TYPE_IWDG = 0,     /*!< Independent Watchdog (LSI clock) */
    CA_WDT_TYPE_WWDG,         /*!< Window Watchdog (APB clock) */
} ca_wdt_type_t;

/* =============================================================================
 * WATCHDOG MODE ENUMERATION
 * ============================================================================= */

typedef enum {
    CA_WDT_MODE_RESET = 0,    /*!< MCU reset only (all families) */
    CA_WDT_MODE_INTERRUPT,    /*!< Early warning interrupt, then reset: see ca_wdt_on_early() */
} ca_wdt_mode_t;

/* Early warning callback: runs in INTERRUPT context, no blocking calls */
typedef void (*ca_wdt_early_cb_t)(void *arg);

/* =============================================================================
 * RESET CAUSE
 * ============================================================================= */

typedef enum {
    CA_WDT_RESET_NONE = 0,    /*!< the last reset was not a watchdog one */
    CA_WDT_RESET_IWDG,        /*!< Independent Watchdog reset */
    CA_WDT_RESET_WWDG,        /*!< Window Watchdog reset */
} ca_wdt_reset_t;

#define CA_WDT_TIMEOUT_MIN_MS   1U    /* the maximum depends on type and family */

/* =============================================================================
 * PUBLIC API
 * ============================================================================= */

/* Configures the watchdog; a running IWDG can be configured again (new
 * timeout, e.g. a short one to reset now) and keeps running. The watchdog is
 * frozen while the core is halted by a debugger. */
bool ca_wdt_init(u32 timeout_ms, ca_wdt_mode_t mode, ca_wdt_type_t type);
/* Starts the watchdog (and reloads it): from here it must be fed */
bool ca_wdt_enable(void);
void ca_wdt_reload(void);
u32  ca_wdt_get_timeout_ms(void);
bool ca_wdt_is_enabled(void);
/* Longest timeout this family supports for that type, in ms */
u32  ca_wdt_max_timeout_ms(ca_wdt_type_t type);
/* Whether the last reset came from a watchdog. The RCC reset flags stay set
 * until cleared: clear = true clears ALL of them (power-on, pin, software...),
 * so a caller that needs the others reads them before. */
ca_wdt_reset_t ca_wdt_reset_cause(bool clear);

/* Early warning (CA_WDT_MODE_INTERRUPT): cb runs shortly before the watchdog
 * resets the MCU - WWDG one counter tick before (every family), IWDG with a
 * quarter of the timeout left (H5/U5: the other families have no IWDG early
 * interrupt, and ca_wdt_init() refuses the mode there). The reset still
 * follows: it is the last chance to save state, not a way to stay alive -
 * reloading from the callback hides the fault that starved the watchdog.
 * Set it before ca_wdt_enable(); NULL removes it. */
void ca_wdt_on_early(ca_wdt_early_cb_t cb, void *arg);
/* Whether this family can warn before the reset with that watchdog */
bool ca_wdt_early_supported(ca_wdt_type_t type);


#endif /* WDT_H */
