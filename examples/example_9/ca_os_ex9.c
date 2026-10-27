// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

#if EN_NOTIFY

/*
 * Example of task notification usage
 *
 * Notifications are the lightest signaling mechanism in caOS:
 * zero memory overhead (they use the notify_value field of the task context)
 * and are faster than queues and semaphores for simple signaling.
 *
 * In this example, a sender task sends notifications to a receiver task,
 * showing the different available actions: overwrite, increment, and set bits.
 */

static u32 running;
static ca_task_handle_t receiver = CA_TASK_HANDLE_NONE;  // receiver, saved for the sender
// A handle, not the idx: idx is a list position and moves when a task below it ends.

// Receiver task: waits for notification from the sender
static u32 task_receiver(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    ca_stdout_printf_n("RECEIVER started: idx=%u p1=%u", idx, p1);

    // saves its own index for the sender
    receiver = ca_os_self_handle();

    while(running) {
        // waits for notification with automatic clear, 2000ms timeout
        u32 value = ca_notify_take(2000);

        if (value != 0) {
            ca_stdout_printf_n("RECEIVER: notification received value=0x%08X", value);
        } else {
            ca_stdout_printf_n("RECEIVER: timeout, no notification");
        }
    }

    return 0;
}

// Sender task: sends notification to the receiver with different actions
static u32 task_sender(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    ca_stdout_printf_n("SENDER started: idx=%u p1=%u", idx, p1);

    u32 cycle = 0;

    while(running) {
        ca_os_sleep(500);

        switch(cycle % 3) {
        case 0:
            // overwrite: writes a direct value
            ca_notify_send_handle(receiver, 0xAA00 + cycle, CA_NOTIFY_ACTION_OVERWRITE);
            ca_stdout_printf_n("SENDER: overwrite value=0x%08X", 0xAA00 + cycle);
            break;

        case 1:
            // increment: increments the receiver's counter
            ca_notify_send_handle(receiver, 0, CA_NOTIFY_ACTION_INCREMENT);
            ca_stdout_printf_n("SENDER: increment");
            break;

        case 2:
            // set bits: sets specific bits
            ca_notify_send_handle(receiver, (1 << 0) | (1 << 4), CA_NOTIFY_ACTION_SET_BITS);
            ca_stdout_printf_n("SENDER: set bits 0x%08X", (1 << 0) | (1 << 4));
            break;
        }

        cycle++;
    }

    return 0;
}


/* ============================================================================
 * 'twait [sec]': timed wait check on the task notification (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task notifies the waiter after 1..8 ms; the waiter, with a
 *             20 ms timeout, must get it and be running within TW_LAT_MS.
 *   timeout : nobody notifies; T = 1..20 ms, never back before T,
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
static volatile ca_task_handle_t tw_waiter = CA_TASK_HANDLE_NONE;
static void tw_init(void) {  }
static void tw_hold(void) {  }
static void tw_give(void) { (void)ca_notify_send_handle(tw_waiter, 1U, CA_NOTIFY_ACTION_INCREMENT); }
static bool tw_wait(u32 ms) { return ca_notify_take(ms) != 0U; }
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
            CLI_PUT_N("TW notify : peer task not created  FAIL");
            return;
        }
    }
    tw_waiter = ca_os_self_handle();
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
    CLI_PRINTF_N("TW notify : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

/* 'setempty': CA_NOTIFY_ACTION_SET_IF_EMPTY on the command's own task, so
 * nothing else can touch the value in between. Empty -> written; pending ->
 * refused (false) and the pending value kept; read -> writable again. */
static void set_if_empty_check(void) {
    const ca_task_handle_t self = ca_os_self_handle();
    const u32 me = ca_scheduler_get_current_task_id();
    (void)ca_notify_clear(me);
    const bool first  = ca_notify_send_handle(self, 0x11U, CA_NOTIFY_ACTION_SET_IF_EMPTY);
    const bool second = ca_notify_send_handle(self, 0x22U, CA_NOTIFY_ACTION_SET_IF_EMPTY);
    const u32  kept   = ca_notify_get(me);
    const u32  taken  = ca_notify_take(1U);
    const bool again  = ca_notify_send_handle(self, 0x33U, CA_NOTIFY_ACTION_SET_IF_EMPTY);
    const u32  last   = ca_notify_get(me);
    (void)ca_notify_clear(me);
    const bool ok = first && !second && kept == 0x11U && taken == 0x11U &&
                    again && last == 0x33U;
    CLI_PRINTF_N("SE notify : first=%u second=%u kept=0x%X taken=0x%X again=%u last=0x%X  %s",
                 (u32)first, (u32)second, kept, taken, (u32)again, last, ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the task notification: latency + timeout");
        CLI_PUT_N("setempty  : CA_NOTIFY_ACTION_SET_IF_EMPTY check: written when empty, refused when pending");
        return false;
    }
    if (ca_strncmp(p, "setempty", 8) == 0) {
        set_if_empty_check();
        return true;
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
    EVAL_NOTIFY(
    running = 1;

    // receiver created first to have its idx available
    ca_os_job("receiver", CA_TH_LEVEL2, task_receiver, NULL,
                          STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job("sender",   CA_TH_LEVEL3, task_sender, NULL,
                          STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    )
    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    ca_os_run(true);
}
#else /* EN_NOTIFY */
/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m9a, "=== Example 9 ===");
PRIVATE_STRING(m9b, "EN_NOTIFY == 0, skipping task notification example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m9a);
    PUT_N(&priv_m9b);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_NOTIFY */
