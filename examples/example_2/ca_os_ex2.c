// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 2 — Multitasking basics
 *
 * Two independent tasks (task_1_exec, task_2_exec) created via ca_os_job()
 * and run by the preemptive scheduler (ca_os_run). Each task allocates its
 * own heap buffer, loops printing a message at a different period, and
 * releases the buffer on exit. Shows job creation, priorities/stack sizing,
 * ca_memory_allocate/free, and STDOUT/LOG/CLI service init in ca_os_configure().
 */

#include "caos.h"


static u32 terminate;


static u32 task_1_exec(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    char *p = ca_memory_allocate(1024);
    int count =0;
    ca_stdout_printf_n("TASK 1: par1=%u", p1);
    while(terminate) {
        count++;
        ca_memset(p, 0, 1024);
        ca_stdout_printf_n("TASK 1: Hi!"); 
        ca_os_sleep(391);
    }
    ca_memory_free(p, NULL, false);
    return 0;
}


static u32 task_2_exec(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    char *p = ca_memory_allocate(512);
    int count =0;
    ca_stdout_printf_n("TASK 2: par1=%u", p1);
    while(terminate) {
        count++;
        ca_memset(p, 0, 50);
        ca_stdout_printf_n("TASK 2: hello!"); 
        ca_os_sleep(457);
    }
    ca_memory_free(p, NULL, false);
    return 0;
}

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
    /* power management: see example_59 (ex59 is the example dedicated to the pwr driver) */
    /* DBG */
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}



void ca_os_main(void) {
    terminate=1;
    // Create tasks
    ca_os_job("t1_w1", CA_TH_LEVEL3, task_1_exec, (void *)(uintptr_t)1,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("t1_w2", CA_TH_LEVEL3, task_2_exec, (void *)(uintptr_t)3,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

