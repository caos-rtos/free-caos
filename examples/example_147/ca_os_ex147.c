// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex147.c
 * @brief          : Example 147 — MicroPython with files on the SD card
 ******************************************************************************
 * @details
 * The REPL of example 145 plus the SD card (SDIO + FatFS through
 * fat_service). At every start of the VM the card is mounted at /sd, the
 * current directory is /sd, and boot.py then main.py run from there if they
 * exist. open(), import of modules on the card and the os module work, so
 * mpremote can copy files (mpremote fs cp).
 *
 *   py             MicroPython REPL (Ctrl-D on an empty line exits)
 *   fat ...        the FAT service commands (mount, ls, mkfs, ...)
 ******************************************************************************
 */

#include "caos.h"

#if EN_PYTHON && ENABLE_CLI && EN_FAT && EN_FAT_SERVICE

void ca_os_configure(void)
{
    EVAL_DMA( ca_dma_init(); )          /* before SDIO */
    /* an absent card is a normal state: the CLI still answers */
    EVAL_SDIO( (void)ca_sdio_init(); )
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
    )
    ca_fat_service_init();              /* after ca_cli_init: registers 'fat' */
    EVAL_CLI( ca_cli_create_thread(); )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else

PRIVATE_STRING(m147a, "=== Example 147 ===");
PRIVATE_STRING(m147b, "ex147: EN_PYTHON, ENABLE_CLI, EN_FAT or EN_FAT_SERVICE disabled");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m147a);
    PUT_N(&priv_m147b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
