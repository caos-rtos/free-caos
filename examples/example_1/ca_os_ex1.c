// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 1 — Hello world (no scheduler)
 *
 * Minimal caOS program: prints a formatted message once per second
 * in an infinite loop, with no task creation and no call to ca_os_run().
 * Shows the bare-metal entry points (ca_os_configure/ca_os_main) and
 * basic formatted output via ca_printf().
 */

#include "caos.h"

/**
 * @brief User modules initialization
 * @details Function called by the system before the scheduler starts.
 *          In this example, there is no need to initialize anything.
 */
void ca_os_configure(void) {
    // No additional initialization
}

/**
 * @brief Main task
 * @details Infinite loop that prints a message every second.
 *          Does not use the scheduler (no call to ca_os_run).
 */
void ca_os_main(void) {
    while(1) {
        u32 E = 0x123;
        u16 V = 0x456;
        u8  T = 0x78;
        char buffer[128];
        ca_printf(STDOUT, buffer, sizeof(buffer),
                  "Hello from CAOS ! E=%08X V=%04x T=%02x\r\n", E, V, T);
        ca_os_sleep(1000U);
    }
}