// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 23 — Internal Flash: CLI integrated in the driver
 *
 * Uses the built-in CLI registered by ca_flash_init():
 *   flash info               — sectors, sizes, flash used/free
 *   flash dump <addr>        — hexdump 256 bytes from address
 *   flash erase <addr> <n>  — erase range by address + size
 *   flash erase_s <s>        — erase sector by number
 *   flash test <s>           — erase + write pattern 0xCAA55ACA + verify
 *
 * ca_flash_irq_init() enables the IRQ-driven infrastructure (semaphore + NVIC)
 * for the ca_flash_erase_sector_irq / ca_flash_write_irq APIs.
 *
 * Hardware: Nucleo-H753ZI — no external components required.
 */

#include "caos.h"

#if EN_FLASH

void ca_os_configure(void)
{
    ca_flash_irq_init();   /* semaphore + NVIC — enables IRQ-driven APIs */

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
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}

void ca_os_main(void)
{
    ca_flash_init();   /* registers the driver CLI node */
    ca_os_run(true);
}

#else  /* EN_FLASH == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m23a, "=== Example 23 ===");
PRIVATE_STRING(m23b, "EN_FLASH == 0 (skipping flash example)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m23a);
    PUT_N(&priv_m23b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_FLASH */
