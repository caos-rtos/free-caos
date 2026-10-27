// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    ca_os_ex13.c
 * @brief   Example 13 — Execution time profiling with DWT CYCCNT
 *
 * Demonstrates the use of the measure module (caos/src/measure.c) to measure
 * the execution time of a function from within a caOS task.
 *
 * The measure module uses the ARM core DWT->CYCCNT counter:
 *   - no HW timer occupied
 *   - 1 clock cycle resolution
 *   - ca_measure_get_value() returns microseconds
 *
 * Sequence: ca_measure_init() → ca_measure_start() → <code>
 *           → ca_measure_stop() → ca_measure_get_value()
 ******************************************************************************
 */

#include "caos.h"

#if EN_MEASURE

/* Results updated by the task, read from the CLI */
static u32 g_elapsed_us;
static volatile u32 g_run_count;

/* ------------------------------------------------------------------ */
/* Function to be profiled                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Synthetic work function — simple calculation loop
 * @note  volatile prevents compiler optimization
 */
static void work_function(void)
{
    volatile u32 x = 1U;
    for (u32 i = 0U; i < 50000U; i++) {
        x = x * 3U + 1U;
    }
    (void)x;
}

/* ------------------------------------------------------------------ */
/* Measure task                                                        */
/* ------------------------------------------------------------------ */

static u32 task_measure_exec(void *arg)
{
    (void)arg;
    u32 elapsed;

    while (1) {
        ca_measure_start(&elapsed);
        work_function();
        ca_measure_stop(&elapsed);
        g_elapsed_us = elapsed;
        g_run_count++;

        ca_os_sleep(1000);
    }

    return 0U;
}


/* ------------------------------------------------------------------ */
/* Blocked loop task                                                   */
/* ------------------------------------------------------------------ */

static u32 task_loop_fail(void *arg)
{
    const u32 p1 = (u32)(uintptr_t)arg;
    ca_os_thread_set_watchdog(p1);
    while (1) {
        while(1){
            ca_os_sleep(10);
        }
        ca_os_thread_reset_watchdog();
    }
    return 0U;
}

/* ------------------------------------------------------------------ */
/* CLI hook                                                            */
/* ------------------------------------------------------------------ */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("show : elapsed time by example_13");
        return false;
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        u32 time_ms=(u32)((g_elapsed_us*1000)/ca_os_clock_get_freq());
        CLI_PRINTF_N("run count : %u", g_run_count);
        CLI_PRINTF_N("last us   : %u clocks %u ms", (u32)g_elapsed_us,time_ms);
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };


/* ------------------------------------------------------------------ */
/* ca_os_configure — called before the scheduler                       */
/* ------------------------------------------------------------------ */

void ca_os_configure(void)
{


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
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    
}

/* ------------------------------------------------------------------ */
/* ca_os_main — user entry point                                       */
/* ------------------------------------------------------------------ */

void ca_os_main(void)
{
    EVAL_MEASURE(
        g_elapsed_us = 0U;
        g_run_count  = 0U;

        
        ca_os_job("measure", CA_TH_LEVEL2, task_measure_exec, NULL,
                            STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

        ca_os_job("loop", CA_TH_LEVEL4, task_loop_fail,(void *)(uintptr_t)16000000,
                            STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);


        EVAL_CLI( ca_cli_register(&s_user_cli_node); )
    )
    ca_os_run(true);
}

#else /* EN_MEASURE */
/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m13a, "=== Example 13 ===");
PRIVATE_STRING(m13b, "EN_MEASUREM == 0, skipping execution time profiling example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m13a);
    PUT_N(&priv_m13b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_MEASURE */
