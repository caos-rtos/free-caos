// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

/*
 * Example of condition variable usage
 * 
 * In this example, we simulate a producer-consumer system that uses
 * a condition variable to coordinate access to a shared resource.
 * We have a shared buffer and two tasks: a producer that inserts data
 * into the buffer and a consumer that extracts them. The condition variable allows
 * suspending the consumer when the buffer is empty and the producer
 * when the buffer is full.
 */

#define BUFFER_SIZE 5
static u32 buffer[BUFFER_SIZE];
static u32 head = 0;  // Head index (next element to read)
static u32 tail = 0;  // Tail index (next element to write)
static u32 count = 0; // Number of elements in the buffer

static u32 running;
static ca_mutex_t buffer_mutex;      // Mutex to protect access to the buffer
static ca_condvar_t buffer_not_full;  // Condition variable for buffer not full
static ca_condvar_t buffer_not_empty; // Condition variable for buffer not empty

// Function to check if the buffer is full
static bool is_buffer_full(void) {
    return count == BUFFER_SIZE;
}

// Function to check if the buffer is empty
static bool is_buffer_empty(void) {
    return count == 0;
}

// Function to insert an element into the buffer
static void put_item(u32 item) {
    if (is_buffer_full()) {
        return; // Buffer full
    }
    buffer[tail] = item;
    tail = (tail + 1) % BUFFER_SIZE;
    count++;
}

// Function to extract an element from the buffer
static u32 get_item(void) {
    if (is_buffer_empty()) {
        return 0; // Buffer empty
    }
    u32 item = buffer[head];
    head = (head + 1) % BUFFER_SIZE;
    count--;
    return item;
}

static u32 producer_task(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    char task_name[32];
    ca_snprintf(task_name, sizeof(task_name), "PRODUCER_%u", idx);

    ca_stdout_printf_n("%s started: idx=%u p1=%u", task_name, idx, p1);
    
    u32 item = 100; // Start producing from 100
    
    while(running) {
        // Acquire the mutex to access the buffer
        ca_mutex_lock(&buffer_mutex);
        
        // Wait until the buffer is full
        while(is_buffer_full()) {
            ca_stdout_printf_n("%s: buffer full, waiting...", task_name);
            // Use cond_wait to release the mutex and wait for someone
            // to signal that the buffer is no longer full
            ca_cond_wait(&buffer_not_full, &buffer_mutex);
        }
        
        // Now we can insert an element into the buffer
        put_item(item);
        ca_stdout_printf_n("%s: put item %u, count=%u", task_name, item, count);
        
        // Signal that the buffer is no longer empty
        ca_cond_signal(&buffer_not_empty);

        // Release the mutex
        ca_mutex_unlock(&buffer_mutex);

        item++; // Next element to produce

        // Wait a bit before producing the next element
        ca_os_sleep(500);
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
        // Acquire the mutex to access the buffer
        ca_mutex_lock(&buffer_mutex);
        
        // Wait until the buffer is empty
        while(is_buffer_empty()) {
            ca_stdout_printf_n("%s: buffer empty, waiting...", task_name);
            // Use cond_wait to release the mutex and wait for someone
            // to signal that the buffer is no longer empty
            ca_cond_wait(&buffer_not_empty, &buffer_mutex);
        }
        
        // Now we can extract an element from the buffer
        u32 item = get_item();
        ca_stdout_printf_n("%s: got item %u, count=%u", task_name, item, count);
        
        // Signal that the buffer is no longer full
        ca_cond_signal(&buffer_not_full);
        
        // Release the mutex
        ca_mutex_unlock(&buffer_mutex);
        
        // Simulate some processing of the element
        ca_os_sleep(800);
    }
    
    return 0;
}


/* ============================================================================
 * 'twait [sec]': timed wait check on the condition variable (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task sets the flag and signals after 1..8 ms; the waiter, with a
 *             20 ms timeout, must get it and be running within TW_LAT_MS.
 *   timeout : nobody signals; T = 1..20 ms, never back before T,
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
static ca_mutex_t tw_mtx;
static ca_condvar_t tw_obj;
static volatile bool tw_flag;
static void tw_init(void) { ca_mutex_init(&tw_mtx); ca_cond_init(&tw_obj); }
static void tw_hold(void) {  }
static void tw_give(void) { ca_mutex_lock(&tw_mtx); tw_flag = true; ca_cond_signal(&tw_obj); ca_mutex_unlock(&tw_mtx); }
static bool tw_wait(u32 ms) { ca_mutex_lock(&tw_mtx);
    if (!tw_flag) (void)ca_cond_timedwait(&tw_obj, &tw_mtx, ms);
    const bool ok = tw_flag;
    tw_flag = false;
    ca_mutex_unlock(&tw_mtx);
    return ok; }
static void tw_after(void) {  }
static void tw_drain(void) {  }

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
            CLI_PUT_N("TW cond : peer task not created  FAIL");
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
    CLI_PRINTF_N("TW cond : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the condition variable: latency + timeout");
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
    
    // Initialize the mutex and the condition variables
    ca_mutex_init(&buffer_mutex);
    ca_cond_init(&buffer_not_full);
    ca_cond_init(&buffer_not_empty);
    
    // Create the tasks
    ca_os_job("prod_1", CA_TH_LEVEL2, producer_task, (void *)(uintptr_t)1,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("cons_1", CA_TH_LEVEL2, consumer_task, (void *)(uintptr_t)2,
                       STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);

    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    ca_os_run(true);
}