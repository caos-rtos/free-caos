// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

/*
 * Example of critical section usage
 *
 * Critical sections temporarily disable interrupts to ensure
 * that a block of code is executed without interruptions.
 * They support nesting: only the first ca_enter_critical() disables IRQs,
 * only the last ca_exit_critical() re-enables them.
 *
 * A writer task updates a shared variable and a reader task reads it.
 * Both use critical sections to ensure data consistency.
 */

static u32 running;
static u32 shared_counter = 0;
static u32 write_count = 0;
static u32 read_count = 0;

// Task that updates the shared variable
static u32 task_writer(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    ca_stdout_printf_n("WRITER started: idx=%u p1=%u", idx, p1);

    while(running) {
        ca_enter_critical();
        shared_counter++;
        write_count++;
        u32 sc = shared_counter;
        u32 wc = write_count;
        u32 nesting = ca_critical_nesting_level();
        ca_exit_critical();

        ca_stdout_printf_n("WRITER: counter=%u writes=%u nesting_was=%u", sc, wc, nesting);
        ca_os_sleep(300);
    }

    return 0;
}

// Task that reads the shared variable
static u32 task_reader(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    ca_stdout_printf_n("READER started: idx=%u p1=%u", idx, p1);

    while(running) {
        ca_enter_critical();
        u32 sc = shared_counter;
        u32 wc = write_count;
        u32 rc = ++read_count;
        ca_exit_critical();

        ca_stdout_printf_n("READER: counter=%u writes=%u reads=%u", sc, wc, rc);
        ca_os_sleep(500);
    }

    return 0;
}


/* init hardware device enabled */
void ca_os_configure(){
    /* UART DBG or ITM ALREADY SET ON SYSTEM IF ENABLED! */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(    
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    /* DBG */
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}


void ca_os_main(void) {
    running = 1;

    ca_os_job("writer", CA_TH_LEVEL2, task_writer, (void *)(uintptr_t)1,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("reader", CA_TH_LEVEL2, task_reader, (void *)(uintptr_t)2,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);

    ca_os_run(true);
}
