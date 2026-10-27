// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 17 — Event Group
 *
 * Demonstrates the use of ca_event_group_t to synchronize tasks based on bit patterns.
 *
 * Scenario:
 *   Three simulated sensors (temperature, pressure, humidity) collect data
 *   asynchronously. An aggregator task waits for ALL sensors to have
 *   published their data (WAIT_ALL), then processes the result.
 *   A watchdog task uses WAIT_ANY to detect any error event.
 *
 * Structure:
 *   task_sensor_temp   — simulates temperature reading, sets BIT_TEMP every 400ms
 *   task_sensor_pres   — simulates pressure reading, sets BIT_PRES every 700ms
 *   task_sensor_hum    — simulates humidity reading, sets BIT_HUM every 550ms
 *   task_aggregator    — WAIT_ALL (TEMP|PRES|HUM), processes data
 *   task_watchdog      — WAIT_ANY (ERR_*), logs alarms + 3s timeout
 *
 * Bit map:
 *   BIT 0: TEMP ready
 *   BIT 1: PRES ready
 *   BIT 2: HUM  ready
 *   BIT 8: ERR_TEMP
 *   BIT 9: ERR_PRES
 *
 * CLI COMMANDS:
 *   err_t  — inject temperature error
 *   err_p  — inject pressure error
 *   show   — print counters and current bits
 *   help   — list commands
 */

#include "caos.h"

#if EN_EVENT

/* ============================================================================
 * Bit definitions
 * ========================================================================== */

#define BIT_TEMP    CA_EVENT_BIT(0)
#define BIT_PRES    CA_EVENT_BIT(1)
#define BIT_HUM     CA_EVENT_BIT(2)
#define BITS_ALL    (BIT_TEMP | BIT_PRES | BIT_HUM)

#define BIT_ERR_TEMP  CA_EVENT_BIT(8)
#define BIT_ERR_PRES  CA_EVENT_BIT(9)
#define BITS_ERR      (BIT_ERR_TEMP | BIT_ERR_PRES)

/* ============================================================================
 * Shared state
 * ========================================================================== */

static ca_event_group_t eg_sensors;   /* sensors: WAIT_ALL */
static ca_event_group_t eg_errors;    /* errors:  WAIT_ANY */

static volatile i32 g_temp;       /* degrees * 10 (e.g. 215 = 21.5 C) */
static volatile u32 g_pres;       /* Pa */
static volatile u32 g_hum;        /* % * 10 */

static volatile u32 aggregations;
static volatile u32 watchdog_alerts;

/* ============================================================================
 * Simulated sensor tasks
 * ========================================================================== */

static u32 task_sensor_temp(void *arg) {
    (void)arg;
    i32 base = 200;  /* 20.0 C */
    while (1) {
        ca_os_sleep(400);
        /* Simulates reading with small variation */
        base += (i32)(ca_os_tick_ms() & 3) - 1;
        if (base < 150) base = 150;
        if (base > 350) base = 350;
        g_temp = base;
        ca_event_set(&eg_sensors, BIT_TEMP);
    }
    return 0;
}

static u32 task_sensor_pres(void *arg) {
    (void)arg;
    u32 base = 101325;
    while (1) {
        ca_os_sleep(700);
        base += (ca_os_tick_ms() & 7);
        if (base > 102000) base = 101325;
        g_pres = base;
        ca_event_set(&eg_sensors, BIT_PRES);
    }
    return 0;
}

static u32 task_sensor_hum(void *arg) {
    (void)arg;
    u32 base = 450;  /* 45.0 % */
    while (1) {
        ca_os_sleep(550);
        base += (ca_os_tick_ms() & 3);
        if (base > 950) base = 450;
        g_hum = base;
        ca_event_set(&eg_sensors, BIT_HUM);
    }
    return 0;
}

/* ============================================================================
 * Aggregator task: WAIT_ALL all three sensor bits
 * ========================================================================== */

static u32 task_aggregator(void *arg) {
    (void)arg;
    while (1) {
        /* Blocks until ALL bits are set (auto-clear = true) */
        u32 bits = ca_event_wait_all(&eg_sensors, BITS_ALL, true, CA_WAIT_FOREVER);
        if (bits != 0) {
            aggregations++;
            ca_stdout_printf_n("AGG #%u  T=%d.%d  P=%u  H=%u.%u",
                aggregations,
                g_temp / 10, (u32)(g_temp > 0 ? g_temp % 10 : (-g_temp) % 10),
                g_pres,
                g_hum / 10, g_hum % 10);
        }
    }
    return 0;
}

/* ============================================================================
 * Watchdog task: WAIT_ANY on error bits with 3s timeout
 * ========================================================================== */

static u32 task_watchdog(void *arg) {
    (void)arg;
    while (1) {
        u32 bits = ca_event_wait_any(&eg_errors, BITS_ERR, true, 3000);
        if (bits == 0) {
            /* timeout: no error in 3s — ok */
            ca_stdout_printf_n("WDG: OK (no errors in 3s)");
        } else {
            watchdog_alerts++;
            if (bits & BIT_ERR_TEMP)
                ca_stdout_printf_n("WDG: ALERT - temperature error! (#%u)", watchdog_alerts);
            if (bits & BIT_ERR_PRES)
                ca_stdout_printf_n("WDG: ALERT - pressure error! (#%u)", watchdog_alerts);
        }
    }
    return 0;
}

/* ============================================================================
 * CLI hook
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("err_t | err_p | show");
        return false;
    }
    if (ca_strncmp(p, "err_t", 5) == 0) {
        ca_event_set(&eg_errors, BIT_ERR_TEMP);
        CLI_PUT_N("Injected ERR_TEMP");
        return true;
    }
    if (ca_strncmp(p, "err_p", 5) == 0) {
        ca_event_set(&eg_errors, BIT_ERR_PRES);
        CLI_PUT_N("Injected ERR_PRES");
        return true;
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        u32 sb = ca_event_get(&eg_sensors);
        u32 eb = ca_event_get(&eg_errors);
        CLI_PUT_N("--- Example 17 ---");
        CLI_PRINTF_N("  Aggregations  : %u", aggregations);
        CLI_PRINTF_N("  WDG alerts    : %u", watchdog_alerts);
        CLI_PRINTF_N("  Sensor bits   : 0x%02X (T=%u P=%u H=%u)",
            sb,
            (sb & BIT_TEMP) ? 1u : 0u,
            (sb & BIT_PRES) ? 1u : 0u,
            (sb & BIT_HUM)  ? 1u : 0u);
        CLI_PRINTF_N("  Error bits    : 0x%02X", eb);
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };



/* ============================================================================
 * 'twait [sec]': timed wait check on the event group (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task sets the bit after 1..8 ms; the waiter, with a
 *             20 ms timeout, must get it and be running within TW_LAT_MS.
 *   timeout : nobody sets it; T = 1..20 ms, never back before T,
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
static ca_event_group_t tw_obj;
static void tw_init(void) { ca_event_init(&tw_obj); }
static void tw_hold(void) {  }
static void tw_give(void) { (void)ca_event_set(&tw_obj, 0x1U); }
static bool tw_wait(u32 ms) { return (ca_event_wait_any(&tw_obj, 0x1U, true, ms) & 0x1U) != 0U; }
static void tw_after(void) {  }
static void tw_drain(void) { ca_event_reset(&tw_obj); }

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
            CLI_PUT_N("TW event : peer task not created  FAIL");
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
    CLI_PRINTF_N("TW event : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the event group: latency + timeout");
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

/* ============================================================================
 * ca_os_configure
 * ========================================================================== */

void ca_os_configure(void) {
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

/* ============================================================================
 * ca_os_main
 * ========================================================================== */

void ca_os_main(void) {
    EVAL_EVENT(
        aggregations    = 0;
        watchdog_alerts = 0;

        ca_event_init(&eg_sensors);
        ca_event_init(&eg_errors);

        ca_os_job("temp",  CA_TH_LEVEL2, task_sensor_temp, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
        ca_os_job("pres",  CA_TH_LEVEL2, task_sensor_pres, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
        ca_os_job("hum",   CA_TH_LEVEL2, task_sensor_hum,  NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
        ca_os_job("aggr",  CA_TH_LEVEL1, task_aggregator,  NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
        ca_os_job("wdog",  CA_TH_LEVEL1, task_watchdog,    NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

        EVAL_CLI( ca_cli_register(&s_user_cli_node); )
    )
    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    ca_os_run(true);
}
#else /* EN_EVENT */
/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m17a, "=== Example 17 ===");
PRIVATE_STRING(m17b, "EN_EVENT == 0, skipping event example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m17a);
    PUT_N(&priv_m17b);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_EVENT */