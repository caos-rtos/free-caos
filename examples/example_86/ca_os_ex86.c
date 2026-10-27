// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 86 - 'callto': calling an arbitrary address from the console
 *
 * The console has always been able to READ any address ('mem', 'hexdump').
 * 'callto' calls one, in a one-shot task of its own, and reports what it
 * returned. This example exists to give that command something with a KNOWN
 * answer to call: three targets, whose addresses it prints at boot so that a
 * test - or a person - can call them without a map file or a debugger.
 *
 *   callto <u32 addr> 15     -> 0x0000002B   (x*2+1 of 0x15)
 *   callto <void addr>       -> 0xCA05CA05
 *   callto <spin addr>       -> never returns: 'taskall' shows it, 'kill' ends it
 *
 * No peripheral, no LED, no wiring: the command is MCU-agnostic and so is this
 * example, which is why it runs on every board.
 *
 * WARNING: EN_CLI_CALLTO is a debug flag. It is arbitrary execution from the
 * console, and a build that ships it hands over whatever the firmware holds -
 * keys_store included. It is on HERE because this is what the example is for.
 */

#include "caos.h"

/* The magic the void target returns. A value nobody would print by accident,
 * so the test cannot pass on a coincidence. */
#define EX86_MAGIC   0xCA05CA05U

/* ---- the targets ------------------------------------------------------- */

/* Takes an argument: proves the parameter reaches r0 and the result comes
 * back. x*2+1 rather than x*2, so an answer of zero can never be mistaken for
 * a correct one. */
static u32 ex86_target_u32(u32 x) {
    return (x * 2U) + 1U;
}

/* Takes none: called through a u32(void), which is what 'callto' does when no
 * parameter is typed. */
static u32 ex86_target_void(void) {
    return EX86_MAGIC;
}

/* Never returns. The point is not to burn the CPU - it sleeps - but to be a
 * call that does not come back, so that the one-shot task is still there to be
 * found in 'taskall' and ended with 'kill'. That is the whole reason the call
 * runs in a task instead of in the CLI thread. */
static u32 ex86_target_spin(void) {
    while (1) {
        ca_os_sleep(100);
    }
    return 0U;   /* not reached */
}

/* ---- example ----------------------------------------------------------- */

static u32 task_main(void *arg) {
    (void)arg;

    /* After the banner, so the boot transcript carries the addresses. A
     * function pointer on Thumb already has bit 0 set: 'callto' accepts the
     * address either way, and these are printed exactly as the compiler sees
     * them. */
    ca_os_sleep(500);
    ca_stdout_put_n("example_86: callto targets");
    ca_stdout_printf_n("ex86: u32  0x%08X  f(x) = x*2+1",
                       (u32)&ex86_target_u32);
    ca_stdout_printf_n("ex86: void 0x%08X  f()  = 0x%08X",
                       (u32)&ex86_target_void, EX86_MAGIC);
    ca_stdout_printf_n("ex86: spin 0x%08X  never returns",
                       (u32)&ex86_target_spin);
    ca_stdout_put_n("example_86: ready");

    while (1) {
        ca_os_sleep(1000);
    }
    return 0;
}

void ca_os_configure(void) {
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    /* 'callto' registers itself from ca_cli_init() when EN_CLI_CALLTO is on:
     * nothing to add here, and nothing to forget. */
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
}

void ca_os_main(void) {
    ca_os_job("example_86", CA_TH_LEVEL3, task_main, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}
