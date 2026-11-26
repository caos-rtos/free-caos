// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex146.c
 * @brief          : Example 146 — MicroPython REPL started at boot
 ******************************************************************************
 * @details
 * EN_UPY_AUTOSTART: caOS starts the MicroPython master task itself, right
 * after ca_os_configure() - the example calls nothing. The REPL takes the
 * console as soon as the boot banner is out, so the board boots to '>>>'.
 * There is no 'py' command: the console belongs to Python from boot. Ctrl-D
 * on an empty line (or sys.exit()) gives the console to the CLI for good
 * (until the next reset).
 ******************************************************************************
 */

#include "caos.h"

#if EN_PYTHON && ENABLE_CLI

void ca_os_configure(void)
{
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else

PRIVATE_STRING(m146a, "=== Example 146 ===");
PRIVATE_STRING(m146b, "ex146: EN_PYTHON or ENABLE_CLI disabled");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m146a);
    PUT_N(&priv_m146b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
