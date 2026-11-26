// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex145.c
 * @brief          : Example 145 — MicroPython REPL on the console
 ******************************************************************************
 * @details
 * EN_PYTHON compiles MicroPython (external/micropython + the caOS port in
 * external/caos_micropython) and registers the 'py' command. 'py' starts the
 * master task and returns: the task takes the console, allocates its GC heap
 * (UPY_HEAP_SIZE) and runs the REPL. Ctrl-C interrupts a running program,
 * Ctrl-D on an empty line (or sys.exit()) ends the REPL and the console goes
 * back to the CLI.
 *
 *   py             MicroPython REPL
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

PRIVATE_STRING(m145a, "=== Example 145 ===");
PRIVATE_STRING(m145b, "ex145: EN_PYTHON or ENABLE_CLI disabled");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m145a);
    PUT_N(&priv_m145b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
