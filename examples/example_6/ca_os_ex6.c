// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

/* 
 * Example of semaphore usage
 * 
 * In this example, we simulate a shared resource control system
 * using semaphores. We have a pool of resources (e.g., database connections,
 * hardware device accesses, etc.) and several tasks that require their use.
 * 
 * The semaphore is used to limit the number of tasks that can access
 * the shared resources simultaneously.
 */

#define NUM_RESOURCES 3  // Number of available resources
#define NUM_PRODUCERS 2  // Number of producer tasks
#define NUM_CONSUMERS 4  // Number of consumer tasks

static u32 running;
static ca_semaphore_t resource_semaphore;  // Semaphore to control access to resources

// Array to track the status of resources
static u32 resource_status[NUM_RESOURCES];
static ca_mutex_t resource_mutex;  // Mutex to protect access to the array

static u32 producer_task(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    char task_name[32];

    ca_snprintf(task_name, sizeof(task_name), "PRODUCER_%u", idx);

    ca_stdout_printf_n("%s started: idx=%u p1=%u", task_name, idx, p1);
    
    while(running) {
        // We wait a bit before requesting a resource
        ca_os_sleep(500 + (idx * 200));  // Different delay for each producer
        
        // We try to acquire a resource (waiting at most 1000ms)
        if (ca_sem_timedwait(&resource_semaphore, 1000)) {
            // We have obtained access to a resource
            ca_stdout_printf_n("%s: acquired resource", task_name);
            
            // We look for a free resource to use
            u32 resource_idx = (u32)-1;
            ca_mutex_lock(&resource_mutex);
            for (int i = 0; i < NUM_RESOURCES; i++) {
                if (resource_status[i] == 0) {  // Free resource
                    resource_status[i] = idx + 1;  // Assign it to this producer
                    resource_idx = i;
                    break;
                }
            }
            ca_mutex_unlock(&resource_mutex);
            
            if (resource_idx != (u32)-1) {
                // We simulate the use of the resource
                ca_stdout_printf_n("%s: using resource %u", task_name, resource_idx);
                
                // Use the resource for a while
                ca_os_sleep(800);
                
                // Free the resource
                ca_mutex_lock(&resource_mutex);
                resource_status[resource_idx] = 0;  // Mark as free
                ca_mutex_unlock(&resource_mutex);
                
                ca_stdout_printf_n("%s: released resource %u", task_name, resource_idx);
            } else {
                ca_stdout_printf_n("%s: ERROR - no free resource found!", task_name);
            }
            
            // We release the semaphore to allow other tasks to access
            ca_sem_post(&resource_semaphore);
        } else {
            // Timeout: we failed to obtain a resource within the time limit
            ca_stdout_printf_n("%s: timeout waiting for resource", task_name);
        }
    }
    
    return 0;
}

static u32 consumer_task(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    char task_name[32];

    ca_snprintf(task_name, sizeof(task_name), "CONSUMER_%u", idx);

    ca_stdout_printf_n("%s started: idx=%u p1=%u", task_name, idx, p1);
    
    while(running) {
        // We wait a bit before checking the status
        ca_os_sleep(700 + (idx * 100));
        
        // We check the status of the resources
        ca_mutex_lock(&resource_mutex);
        u32 busy_resources = 0;
        for (int i = 0; i < NUM_RESOURCES; i++) {
            if (resource_status[i] != 0) {
                busy_resources++;
            }
        }
        ca_mutex_unlock(&resource_mutex);
        
        ca_stdout_printf_n("%s: %u/%u resources busy", task_name, busy_resources, NUM_RESOURCES);
        
        // We check the semaphore value
        u32 sem_value = ca_sem_getvalue(&resource_semaphore);
        ca_stdout_printf_n("%s: semaphore value: %u", task_name, sem_value);
    }
    
    return 0;
}


/* ============================================================================
 * 'twait [sec]': timed wait check on the semaphore (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task posts after 1..8 ms; the waiter, with a
 *             20 ms timeout, must get it and be running within TW_LAT_MS.
 *   timeout : nobody posts; T = 1..20 ms, never back before T,
 *             at most TW_SLACK_MS after it.
 * One line, ends with PASS or FAIL.
 * ========================================================================== */
#if ENABLE_CLI

#define TW_T_MS      20U
#define TW_LAT_MS    2U
#define TW_SLACK_MS  2U

/* 0 idle, 1 arm, 3 armed, 2 signalled, 4 hold, 5 held, 6 release */
static volatile u8   tw_state;
static volatile u32  tw_stamp;
static volatile bool tw_busy;
static bool          tw_ready;
static ca_semaphore_t tw_obj;
static void tw_init(void) { ca_sem_init(&tw_obj, "TW_SEM_", 0, 16); }
static void tw_hold(void) {  }
static void tw_give(void) { ca_sem_post(&tw_obj); }
static bool tw_wait(u32 ms) { return ca_sem_timedwait(&tw_obj, ms); }
static void tw_after(void) {  }
static void tw_drain(void) { while (ca_sem_trywait(&tw_obj)) { } }

static u32 task_tw_peer(void *arg) {
    (void)arg;
    u32 rng = 0x7A17E000U;
    while (1) {
        const u8 st = tw_state;
        if (st == 1U) {
            tw_hold();
            tw_state = 3U;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            ca_os_sleep(1U + rng % 8U);
            tw_stamp = ca_os_tick();
            tw_state = 2U;
            tw_give();
        } else if (st == 4U) {
            tw_hold();
            tw_state = 5U;
        } else if (st == 6U) {
            tw_give();
            tw_state = 0U;
        } else {
            ca_os_sleep(1);
        }
    }
    return 0;
}

/* Waits for the peer to reach @p st, false after 100 ms */
static bool tw_until(u8 st) {
    for (u32 t = 0U; tw_state != st && t < 100U; t++) {
        ca_os_sleep(1);
    }
    return tw_state == st;
}

/* Waits for the peer to arm (3) - or to have signalled already (2): its
 * 1 ms sleep can end in the same tick as the waiter's poll and run first,
 * and 3 is then never seen (equal deadlines, the CFS tie decides) */
static bool tw_until_armed(void) {
    for (u32 t = 0U; tw_state != 3U && tw_state != 2U && t < 100U; t++) {
        ca_os_sleep(1);
    }
    return tw_state == 3U || tw_state == 2U;
}

static void tw_run(u32 sec) {
    if (!tw_ready) {
        tw_init();
        tw_ready = ca_os_job("tw_peer", CA_TH_LEVEL2, task_tw_peer, NULL, STACK_SMALL,
                             CA_JOB_STD, CA_FAULT_ASSERT) != CA_TASK_HANDLE_NONE;
        if (!tw_ready) {
            CLI_PUT_N("TW sem : peer task not created  FAIL");
            return;
        }
    }
    
    const u32 lat_lim = ca_os_tick_x_ms(TW_LAT_MS);
    const u32 slack   = ca_os_tick_x_ms(TW_SLACK_MS);
    u32 rounds = 0U, lost = 0U, spur = 0U, stuck = 0U, lat_max = 0U;

    /* latency: the peer signals, the waiter is parked with a deadline */
    for (u32 r = 0U; r < sec * 100U && stuck == 0U; r++) {
        tw_state = 1U;
        if (!tw_until_armed()) { stuck++; break; }
        const bool ok = tw_wait(TW_T_MS);
        const u32 now = ca_os_tick();
        if (!ok) {
            lost++;
        } else if (tw_state != 2U) {
            spur++;                         /* served before the signal */
        } else if (now - tw_stamp > lat_max) {
            lat_max = now - tw_stamp;
        }
        if (ok) tw_after();
        if (!tw_until(2U)) { stuck++; break; }
        tw_drain();
        tw_state = 0U;
        rounds++;
    }

    /* timeout: nobody signals */
    u32 n = 0U, early = 0U, late = 0U, got = 0U, over_max = 0U;
    
    const u32 end = ca_os_tick() + ca_os_tick_x_ms(sec * 1000U);
    while (stuck == 0U && (i32)(ca_os_tick() - end) < 0) {
        const u32 t_ms = 1U + (n % 20U);
        const u32 need = ca_os_tick_x_ms(t_ms);
        const u32 t0   = ca_os_tick();
        if (tw_wait(t_ms)) { got++; tw_after(); }
        const u32 dt = ca_os_tick() - t0;
        if (dt < need) {
            early++;
        } else {
            if (dt - need > over_max) over_max = dt - need;
            if (dt - need > slack) late++;
        }
        n++;
    }
    

    const bool ok = stuck == 0U && rounds == sec * 100U && lost == 0U && spur == 0U &&
                    lat_max <= lat_lim && n >= sec * 50U && early == 0U && late == 0U &&
                    got == 0U;
    CLI_PRINTF_N("TW sem : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the semaphore: latency + timeout");
        return false;
    }
    if (ca_strncmp(p, "twait", 5) != 0) return false;
    p = ca_skip_sp(p + 5);
    u32 sec = 3U;
    if (*p >= '0' && *p <= '9') {
        const i32 s = ca_strtol(p, 10);
        if (s >= 1 && s <= 30) sec = (u32)s;
    }
    if (tw_busy) {
        CLI_PUT_N("twait: a check is running - wait for its result");
        return true;
    }
    tw_busy = true;
    tw_run(sec);
    tw_busy = false;
    return true;
}
static ca_cli_node_t s_tw_cli_node = { tw_cli_handler, NULL };

#endif /* ENABLE_CLI */

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
    
    // Initialize the semaphore with NUM_RESOURCES available
    ca_sem_init(&resource_semaphore,"EX6_S1", NUM_RESOURCES, NUM_RESOURCES);
    
    // Initialize the mutex to protect access to the resource array
    ca_mutex_init(&resource_mutex);
    
    // Initialize the resource status
    for (int i = 0; i < NUM_RESOURCES; i++) {
        resource_status[i] = 0;  // All resources are free at the beginning
    }
    
    // Create the producer tasks
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        char task_name[16];
        ca_snprintf(task_name, sizeof(task_name), "prod_%d", i+1);
        ca_os_job(task_name, CA_TH_LEVEL2, producer_task, (void *)(uintptr_t)i,
                           STACK_LARGE, CA_JOB_STD, CA_FAULT_ASSERT);
    }
    
    // Create the consumer tasks
    for (int i = 0; i < NUM_CONSUMERS; i++) {
        char task_name[16];
        ca_snprintf(task_name, sizeof(task_name), "cons_%d", i+1);
        ca_os_job(task_name, CA_TH_LEVEL2, consumer_task, (void *)(uintptr_t)i,
                           STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    }

    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    ca_os_run(true);
}