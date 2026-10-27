// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/**
 * @file    ca_os_ex10.c
 * @brief   Test 10: Watchdog Timer (WDT)
 * @details The wdt driver on the bench: configuration, a running IWDG
 *          configured again, and the reset itself - which is the only real
 *          proof that a watchdog works.
 *
 * Idle at boot. The boot prints why the part last reset (and clears the RCC
 * reset flags, so the next boot says it again only if it happens again):
 *     E10: last reset by watchdog: IWDG | WWDG | none
 *
 * CLI "e10":
 *   e10 start          ONE pass, no reset, ends with
 *                        RUN RESULT : OK  (4/4)  /  RUN RESULT : FAIL  (n/4)
 *                      1 range: 0 ms and max+1 ms refused
 *                      2 max IWDG timeout at least 20 s on every family
 *                      3 IWDG at 20 s: effective timeout within 5%, enabled,
 *                        and the feeder keeps the part alive 3 s
 *                      4 the RUNNING IWDG configured again at 5 s: accepted,
 *                        and the part outlives 6 s (> 5 s) on the feeder
 *                      From here the IWDG runs: the feeder task keeps it fed.
 *   e10 bite iwdg [ms] the IWDG at ms (default 2000), feeder stopped: resets
 *   e10 bite wwdg      the WWDG at its longest, never fed: resets. Refused
 *                      once 'start' has the IWDG running (one type at a time)
 *   e10 early iwdg [ms] / e10 early wwdg
 *                      CA_WDT_MODE_INTERRUPT: the early warning callback
 *                      leaves a mark in the last backup register, the
 *                      watchdog is starved and must STILL reset; the next boot
 *                      prints "E10: early warning before the reset: <type>".
 *                      Where the hardware has no early interrupt (IWDG on
 *                      F1/F4/F7/H7) the mode must be refused instead:
 *                      "E10: no early warning for <type> here, mode refused"
 *   e10 info           reset cause at boot, maxima, driver state
 * The CLI 'reboot' (cli.c) arms the IWDG at 1 ms: it must reset too, and the
 * next boot must say IWDG.
 *
 * With a debugger attached the watchdogs stop while the core is halted.
 */

#include "caos.h"

#if EN_WDT

#define E10_LONG_MS        20000U   /* phase 3 */
#define E10_RECONF_MS       5000U   /* phase 4 */
#define E10_FEED_MS          200U   /* feeder period */
#define E10_BITE_DEF_MS     2000U

static const char *const s_cause_name[] = { "none", "IWDG", "WWDG" };

static ca_wdt_reset_t   s_boot_cause = CA_WDT_RESET_NONE;
static volatile bool    s_feed = false;     /* the feeder reloads only when set */
static volatile bool    s_busy = false;
static ca_semaphore_t   s_start_sem;        /* posted by "e10 start" */

/* Early warning mark, in the last backup register (it survives the reset;
 * 16 bits are enough, F1 registers are 16 bits wide): 0xE1, type, calls */
#define E10_MARK            0xE100U
#define E10_MARK_MASK       0xFF00U
static volatile u32     s_early_calls = 0U;

static u32 bkp_mark_idx(void) { return ca_bkp_count() - 1U; }

/* Interrupt context, just before the reset: record and return */
static void early_cb(void *arg)
{
    const u32 type = (u32)arg;
    s_early_calls++;
    u32 n = s_early_calls > 15U ? 15U : s_early_calls;
    (void)ca_bkp_write(bkp_mark_idx(), E10_MARK | (type << 4) | n);
}

/* =============================================================================
 * Feeder: the only task that reloads the watchdog
 * ============================================================================= */
static u32 task_feeder(void *arg)
{
    (void)arg;
    while (1) {
        if (s_feed) ca_wdt_reload();
        ca_os_sleep(E10_FEED_MS);
    }
    return 0;
}

/* =============================================================================
 * "e10 start": one deterministic pass
 * ============================================================================= */
static bool phase(u32 n, bool ok, const char *what)
{
    ca_stdout_printf_n("E10: phase %u %s - %s", n, ok ? "OK" : "FAIL", what);
    return ok;
}

static bool near(u32 got, u32 want)        /* within 5% */
{
    u32 d = got > want ? got - want : want - got;
    return d * 20U <= want;
}

static u32 task_start(void *arg)
{
    (void)arg;
    while (1) {
        ca_sem_wait(&s_start_sem);
        u32 ok = 0U;
        const u32 max_i = ca_wdt_max_timeout_ms(CA_WDT_TYPE_IWDG);

        ca_stdout_put_n("E10: start - 4 phases");

        /* 1: out of range refused (the driver prints its own error line) */
        bool r0 = ca_wdt_init(0U, CA_WDT_MODE_RESET, CA_WDT_TYPE_IWDG);
        bool r1 = ca_wdt_init(max_i + 1U, CA_WDT_MODE_RESET, CA_WDT_TYPE_IWDG);
        ok += phase(1, !r0 && !r1, "0 ms and max+1 ms refused");

        /* 2: the hardware maximum, not an arbitrary cap */
        ca_stdout_printf_n("E10: max IWDG %u ms, WWDG %u ms", max_i,
                           ca_wdt_max_timeout_ms(CA_WDT_TYPE_WWDG));
        ok += phase(2, max_i >= E10_LONG_MS, "IWDG reaches 20 s");

        /* 3: long timeout, enabled, fed */
        bool p3 = ca_wdt_init(E10_LONG_MS, CA_WDT_MODE_RESET, CA_WDT_TYPE_IWDG);
        u32 t3 = ca_wdt_get_timeout_ms();
        s_feed = true;
        p3 = p3 && near(t3, E10_LONG_MS) && ca_wdt_enable() && ca_wdt_is_enabled();
        ca_stdout_printf_n("E10: IWDG %u ms -> effective %u ms, running", E10_LONG_MS, t3);
        ca_os_sleep(3000);
        ok += phase(3, p3, "IWDG at 20 s enabled and fed");

        /* 4: the running IWDG takes a new timeout; outliving it proves both
         * that it took and that the feeder holds it */
        bool p4 = ca_wdt_init(E10_RECONF_MS, CA_WDT_MODE_RESET, CA_WDT_TYPE_IWDG);
        u32 t4 = ca_wdt_get_timeout_ms();
        p4 = p4 && near(t4, E10_RECONF_MS) && ca_wdt_enable();
        ca_stdout_printf_n("E10: running IWDG set to %u ms -> effective %u ms", E10_RECONF_MS, t4);
        ca_os_sleep(E10_RECONF_MS + 1000U);
        ok += phase(4, p4, "running IWDG reconfigured, still alive");

        ca_stdout_printf_n("  RUN RESULT : %s  (%u/4)", ok == 4U ? "OK" : "FAIL", ok);
        s_busy = false;
    }
    return 0;
}

/* =============================================================================
 * "e10 bite": let a watchdog reset the part
 * ============================================================================= */
static void bite(ca_wdt_type_t type, u32 ms)
{
    if (!ca_wdt_init(ms, CA_WDT_MODE_RESET, type)) {
        CLI_PUT_N("e10 bite: the watchdog refused this configuration");
        return;
    }
    s_feed = false;
    if (!ca_wdt_enable()) {
        CLI_PUT_N("e10 bite: enable FAILED");
        return;
    }
    CLI_PRINTF_N("E10: starving %s, %u ms - a reset must follow",
                 type == CA_WDT_TYPE_IWDG ? "IWDG" : "WWDG", ca_wdt_get_timeout_ms());
}

/* "e10 early": the warning first, then the reset - or the mode refused where
 * this watchdog has no early interrupt */
static void early(ca_wdt_type_t type, u32 ms)
{
    const char *name = type == CA_WDT_TYPE_IWDG ? "IWDG" : "WWDG";
    if (!ca_wdt_early_supported(type)) {
        bool took = ca_wdt_init(ms, CA_WDT_MODE_INTERRUPT, type);
        CLI_PRINTF_N("E10: no early warning for %s here, mode %s", name,
                     took ? "ACCEPTED (bug)" : "refused");
        return;
    }
    (void)ca_bkp_write(bkp_mark_idx(), 0U);
    s_early_calls = 0U;
    ca_wdt_on_early(early_cb, (void *)(u32)type);
    if (!ca_wdt_init(ms, CA_WDT_MODE_INTERRUPT, type)) {
        CLI_PUT_N("e10 early: the watchdog refused this configuration");
        return;
    }
    s_feed = false;
    if (!ca_wdt_enable()) {
        CLI_PUT_N("e10 early: enable FAILED");
        return;
    }
    CLI_PRINTF_N("E10: starving %s with early warning, %u ms - warning, then a reset",
                 name, ca_wdt_get_timeout_ms());
}

/* =============================================================================
 * CLI "e10"
 * ============================================================================= */
static bool ex10_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e10       : start | bite iwdg [ms] | bite wwdg | early iwdg [ms] | early wwdg | info");
        return false;
    }
    if (ca_strncmp(p, "e10", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "start", 5) == 0) {
        if (s_busy) { CLI_PUT_N("e10: a pass is already running"); return true; }
        s_busy = true;
        ca_sem_post(&s_start_sem);
    } else if (ca_strncmp(p, "bite", 4) == 0) {
        p = ca_skip_sp(p + 4);
        if (s_busy) {
            CLI_PUT_N("e10: wait for the pass to end");
        } else if (ca_strncmp(p, "iwdg", 4) == 0) {
            p = ca_skip_sp(p + 4);
            u32 ms = 0U;
            while (*p >= '0' && *p <= '9') ms = ms * 10U + (u32)(*p++ - '0');
            if (ms == 0U) ms = E10_BITE_DEF_MS;
            bite(CA_WDT_TYPE_IWDG, ms);
        } else if (ca_strncmp(p, "wwdg", 4) == 0) {
            bite(CA_WDT_TYPE_WWDG, ca_wdt_max_timeout_ms(CA_WDT_TYPE_WWDG));
        } else {
            CLI_PUT_N("e10 bite: iwdg [ms] | wwdg");
        }
    } else if (ca_strncmp(p, "early", 5) == 0) {
        p = ca_skip_sp(p + 5);
        if (s_busy) {
            CLI_PUT_N("e10: wait for the pass to end");
        } else if (ca_strncmp(p, "iwdg", 4) == 0) {
            p = ca_skip_sp(p + 4);
            u32 ms = 0U;
            while (*p >= '0' && *p <= '9') ms = ms * 10U + (u32)(*p++ - '0');
            if (ms == 0U) ms = E10_BITE_DEF_MS;
            early(CA_WDT_TYPE_IWDG, ms);
        } else if (ca_strncmp(p, "wwdg", 4) == 0) {
            early(CA_WDT_TYPE_WWDG, ca_wdt_max_timeout_ms(CA_WDT_TYPE_WWDG));
        } else {
            CLI_PUT_N("e10 early: iwdg [ms] | wwdg");
        }
    } else if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("e10: last reset by watchdog (at boot): %s", s_cause_name[s_boot_cause]);
        CLI_PRINTF_N("e10: max IWDG %u ms, WWDG %u ms",
                     ca_wdt_max_timeout_ms(CA_WDT_TYPE_IWDG),
                     ca_wdt_max_timeout_ms(CA_WDT_TYPE_WWDG));
        CLI_PRINTF_N("e10: %s, timeout %u ms, feeder %s",
                     ca_wdt_is_enabled() ? "running" : "not started",
                     ca_wdt_get_timeout_ms(), s_feed ? "on" : "off");
    } else {
        CLI_PUT_N("e10: start | bite iwdg [ms] | bite wwdg | early iwdg [ms] | early wwdg | info");
    }
    return true;
}

static ca_cli_node_t s_ex10_cli_node = { ex10_cli_handler, NULL };

/* =============================================================================
 * caOS hooks
 * ============================================================================= */
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
        ca_cli_register(&s_ex10_cli_node);
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
    ca_sem_init(&s_start_sem, "E10GO", 0U, 1U);
}

void ca_os_main(void)
{
    char buf[DEFAULT_MSG_SIZE];

    /* read and clear: the next boot reports a watchdog only if one fires again */
    s_boot_cause = ca_wdt_reset_cause(true);
    /* and the early warning mark left by the callback before that reset */
    const u32 mark = ca_bkp_read(bkp_mark_idx());
    (void)ca_bkp_write(bkp_mark_idx(), 0U);

    PRINTF_N(buf, sizeof(buf), "\r\n=================================================");
    PRINTF_N(buf, sizeof(buf), "       TEST 10: Watchdog Timer (WDT)");
    PRINTF_N(buf, sizeof(buf), "=================================================");
    PRINTF_N(buf, sizeof(buf), "E10: last reset by watchdog: %s", s_cause_name[s_boot_cause]);
    if ((mark & E10_MARK_MASK) == E10_MARK)
        PRINTF_N(buf, sizeof(buf), "E10: early warning before the reset: %s, %u call(s)",
                 ((mark >> 4) & 0xFU) == (u32)CA_WDT_TYPE_IWDG ? "IWDG" : "WWDG", mark & 0xFU);
    else
        PRINTF_N(buf, sizeof(buf), "E10: early warning before the reset: none");
    PRINTF_N(buf, sizeof(buf), "E10: max IWDG %u ms, WWDG %u ms",
             ca_wdt_max_timeout_ms(CA_WDT_TYPE_IWDG), ca_wdt_max_timeout_ms(CA_WDT_TYPE_WWDG));
    PRINTF_N(buf, sizeof(buf), "E10: idle - e10 start | bite | early | info");

    ca_os_job("feeder", CA_TH_LEVEL2, task_feeder, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("e10", CA_TH_LEVEL3, task_start, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else /* EN_WDT */
/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m10a, "=== Example 10 ===");
PRIVATE_STRING(m10b, "EN_WDT == 0, skipping watchdog example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m10a);
    PUT_N(&priv_m10b);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_WDT */
