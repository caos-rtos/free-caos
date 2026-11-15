// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * app_5 - XSPI2 trampoline for the NUCLEO-H7S3L8 (H773)
 *
 * The H7S3 has 64 KB of internal flash, and no caOS image fits in it: the
 * images run from the Octo-SPI flash on XSPI2 (XIP, 0x70000000). The
 * trampoline is the small program in the internal flash that maps XSPI2 and
 * jumps to the image; it also has a serial monitor (mem / hexdump, as the
 * caOS CLI) for bring-up.
 *
 * On H773 the trampoline is trampoline_h773.c ALONE, built by
 * workspace/h7rsxx/h773/CMakeLists.txt with its own linker script and
 * without the caOS libraries (no kernel, no scheduler). This file is the
 * entry point every other board builds: there app_5 has nothing to do.
 */

#include "caos.h"

#if MCU_ID != 773

PRIVATE_STRING(m5a, "app_5: XSPI2 trampoline of the NUCLEO-H7S3L8 (H773) only");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m5a);
}

void ca_os_main(void) { ca_os_run(true); }

#endif /* MCU_ID != 773 */
