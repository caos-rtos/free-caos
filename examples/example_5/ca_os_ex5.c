// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 5 — Recursive mutex
 *
 * Two tasks contend for a ca_recursive_mutex_t. task_1_exec locks it and calls
 * a recursive_function() that re-locks it at each recursion depth, showing
 * the lock count growing and shrinking; task_2_exec tries to acquire the
 * same mutex concurrently to show it blocks until fully released. Shows
 * ca_rmutex_lock/unlock and ca_rmutex_get_count usage.
 */

#include "caos.h"

static u32 running;
static ca_recursive_mutex_t rmtx;

// Recursive function that uses the recursive mutex
static void recursive_function(const char* task_name, int depth) {
    // Attempt to acquire the recursive mutex
    ca_rmutex_lock(&rmtx);
    
    // Print the recursion level and the task name
    ca_stdout_printf_n("%s: entered recursion level %d, lock count: %u", 
                      task_name, depth, ca_rmutex_get_count(&rmtx));
    
    // If we are at a certain recursion level, call recursively
    if (depth > 0) {
        recursive_function(task_name, depth - 1);
    }
    
    // Print when exiting the recursion level
    ca_stdout_printf_n("%s: exiting recursion level %d, lock count: %u", 
                      task_name, depth, ca_rmutex_get_count(&rmtx));
    
    // Release the mutex (attention: since it is recursive, 
    // we must release as many times as the locks acquired)
    ca_rmutex_unlock(&rmtx);
}

static u32 task_1_exec(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    ca_stdout_printf_n("TASK recursive_1 started: idx=%u p1=%u", idx, p1);
    
    while(running) {
        // Acquire the recursive mutex
        ca_rmutex_lock(&rmtx);
        ca_stdout_printf_n("TASK recursive_1 acquired mutex, lock count: %u", 
                          ca_rmutex_get_count(&rmtx));
        
        // Call the recursive function with a certain level
        recursive_function("TASK recursive_1", 2);
        
        ca_stdout_printf_n("TASK recursive_1 about to release mutex, lock count: %u", 
                          ca_rmutex_get_count(&rmtx));
        
        // Release the recursive mutex (it will actually be released 
        // only when the lock count returns to 0)
        ca_rmutex_unlock(&rmtx);
        
        ca_os_sleep(1000);
    }
    return 0;
}

static u32 task_2_exec(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());
    ca_stdout_printf_n("TASK recursive_2 started: idx=%u p1=%u", idx, p1);
    
    while(running) {
        // Small pause to allow the first task to obtain the mutex
        ca_os_sleep(300);
        
        // Try to acquire the recursive mutex
        if (ca_rmutex_trylock(&rmtx)) {
            ca_stdout_printf_n("TASK recursive_2 acquired mutex, lock count: %u", 
                              ca_rmutex_get_count(&rmtx));
            
            // Simulate some work
            ca_stdout_printf_n("TASK recursive_2 doing some work...");
            
            // Release the recursive mutex
            ca_rmutex_unlock(&rmtx);
        } else {
            ca_stdout_printf_n("TASK recursive_2: mutex busy (owner may be in recursion)");
        }
        
        ca_os_sleep(800);
    }
    return 0;
}


/* ============================================================================
 * 'twait [sec]': timed wait check on the recursive mutex (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task releases the mutex it holds after 1..8 ms; the waiter, with a
 *             20 ms timeout, must get it and be running within TW_LAT_MS.
 *   timeout : the peer holds the mutex throughout; T = 1..20 ms, never back before T,
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
static ca_recursive_mutex_t tw_obj;
static void tw_init(void) { ca_rmutex_init(&tw_obj); }
static void tw_hold(void) { ca_rmutex_lock(&tw_obj); }
static void tw_give(void) { ca_rmutex_unlock(&tw_obj); }
static bool tw_wait(u32 ms) { return ca_rmutex_timedlock(&tw_obj, ms); }
static void tw_after(void) { ca_rmutex_unlock(&tw_obj); }
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
            CLI_PUT_N("TW rmutex : peer task not created  FAIL");
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
    tw_state = 4U;
    if (!tw_until(5U)) stuck++;
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
    tw_state = 6U;
    (void)tw_until(0U);

    const bool ok = stuck == 0U && rounds == sec * 100U && lost == 0U && spur == 0U &&
                    lat_max <= lat_lim && n >= sec * 50U && early == 0U && late == 0U &&
                    got == 0U;
    CLI_PRINTF_N("TW rmutex : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the recursive mutex: latency + timeout");
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
    
    // Initialize the recursive mutex
    ca_rmutex_init(&rmtx);
    
    // Create the tasks
    ca_os_job("rec_1", CA_TH_LEVEL2, task_1_exec, (void *)(uintptr_t)1,
                       STACK_LARGE, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("rec_2", CA_TH_LEVEL2, task_2_exec, (void *)(uintptr_t)3,
                       STACK_LARGE, CA_JOB_STD, CA_FAULT_ASSERT);

    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    ca_os_run(true);
}