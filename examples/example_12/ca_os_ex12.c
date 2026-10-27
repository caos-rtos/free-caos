// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

static u32 killer;
static u32 shared_counter;
static u32 counter_t1;
static u32 counter_t2;
static u32 counter_t3;
static ca_mutex_t mtx;

/**
 * @brief Task 1: increments counters and competes for mutex
 */
static u32 task_1_exec(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    ca_os_sleep(700);
    ca_stdout_printf_n("TASK t1_w1 started: idx=%u p1=%u", idx, p1);
    while(killer) {
        counter_t1++;
        if (ca_mutex_trylock(&mtx)) {
            shared_counter++;
            ca_mutex_unlock(&mtx);
        }
        ca_os_sleep(500);
    }
    return 0;
}

/**
 * @brief Task 2: increments counters and competes for mutex
 */
static u32 task_2_exec(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    ca_os_sleep(700);
    ca_stdout_printf_n("TASK t1_w2 started: idx=%u p1=%u", idx, p1);
    while(killer) {
        counter_t2++;
        if (ca_mutex_trylock(&mtx)) {
            shared_counter++;
            ca_mutex_unlock(&mtx);
        }
        ca_os_sleep(700);
    }
    return 0;
}

/**
 * @brief Task 3: increments counters and competes for mutex
 */
static u32 task_3_exec(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    ca_os_sleep(50);
    ca_stdout_printf_n("TASK t1_w3 started: idx=%u p1=%u", idx, p1);
    while(killer) {
        counter_t3++;
        if (ca_mutex_trylock(&mtx)) {
            shared_counter++;
            ca_mutex_unlock(&mtx);
        }
        ca_os_sleep(900);
    }
    return 0;
}

/**
 * @brief CLI handler for user commands (chain of responsibility)
 */
static bool user_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("show : show task counters");
        return false;   /* continue chain: show other help as well */
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        CLI_PUT_N("--- task counters ---");
        CLI_PRINTF_N("shared  : %u", (u32)shared_counter);
        CLI_PRINTF_N("t1_w1   : %u", (u32)counter_t1);
        CLI_PRINTF_N("t1_w2   : %u", (u32)counter_t2);
        CLI_PRINTF_N("t1_w3   : %u", (u32)counter_t3);
        return true;
    }
    return false;
}

static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/**
 * @brief User modules initialization
 * @details Configures sensors, logs, and CLI before the scheduler.
 */
void ca_os_configure(void) {
    /* Initialize temperature sensor */
    
    /* Log system */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    /* CLI */
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    /* Onboard LEDs: registers the "led" CLI node, so it has to run after
     * ca_cli_init() above - the flags alone leave the command unknown. */
    EVAL_LED( ca_led_init(); )
    /* DBG */
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}

/**
 * @brief Main task
 * @details Creates user tasks, registers CLI hooks, and starts the scheduler.
 */
void ca_os_main(void) {
    killer = 1;
    shared_counter = counter_t1=counter_t2=counter_t3=0;
    
#if ( MCU==F1xx &&  MCU_ID==103)
    ca_os_job("t1_w1", CA_TH_LEVEL2, task_1_exec, (void *)(uintptr_t)1,
                       STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("t1_w2", CA_TH_LEVEL2, task_2_exec, (void *)(uintptr_t)3,
                       STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("t1_w3", CA_TH_LEVEL2, task_3_exec, (void *)(uintptr_t)5,
                       STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
#else
    ca_os_job("t1_w1", CA_TH_LEVEL2, task_1_exec, (void *)(uintptr_t)1,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("t1_w2", CA_TH_LEVEL2, task_2_exec, (void *)(uintptr_t)3,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("t1_w3", CA_TH_LEVEL2, task_3_exec, (void *)(uintptr_t)5,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
#endif
    
    EVAL_CLI( ca_cli_register(&s_user_cli_node); )
    ca_os_run(true);
}
