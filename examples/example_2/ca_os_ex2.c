// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 2 — Multitasking: round robin and CFS seen from the counters
 *
 * Idle at boot: it waits for a command.
 *
 *   same <lvl> [load%|busy]  as many workers as fit, all at CA_TH_LEVEL<lvl>
 *   multi [load%|busy]       as many workers as fit, spread over the 7 levels
 *                            (N/7 each, the rest from the lowest level up)
 *   get                      counters, CPU load, per level ratios
 *   stop                     ends the workers
 *
 * Every worker runs the same loop: it burns `busy` cycles of ITS OWN CPU time
 * (the runtime the scheduler books to the task, in DWT cycles - it does not
 * grow while other tasks run), counts one round, then sleeps until the start
 * of its next period of E2_PERIOD_MS (a late round does not sleep). Alone, a worker asks load/N of the CPU, so all of
 * them together ask `load` (default 200 %). Since a round costs every worker
 * the same CPU, its counter is the share of CPU it received.
 *
 * What to look at with 'get':
 *  - load below 100 % (e.g. 'multi 50'): idle > 0 ("rest" in 'get' also holds
 *    the scheduler and the interrupts), every worker gets what it
 *    asks, the counters are equal whatever the level - priority does not take
 *    CPU away when there is enough for everybody;
 *  - load above 100 % ('same 3', 'multi'): idle ~ 0, the scheduler shares the
 *    CPU. Same level: equal counters (round robin). Different levels: the
 *    counters grow with the weight of the level (CFS); a level that gets all
 *    it asks (one round per period) stops there, and what is left is shared
 *    by weight among the others (H773, 'multi': L0..L4 at 1:2:4:8:16, L5 and
 *    L6 both at their full demand);
 *  - 'busy' instead of a load: no sleep at all, the ratios follow the weights
 *    1:2:4:...:64 closely.
 */

#include "caos.h"

#if ENABLE_CLI

#define E2_PERIOD_MS      10U
#define E2_LOAD_DEFAULT   200U
#define E2_LEVELS         7U
#define E2_BUSY_UNIT_US   1000U    /* 'busy' mode: one round = 1 ms of CPU */
#define E2_WINDOW_MS      500U     /* 'get' measures the CPU load over this */
#define E2_STOP_MS        2000U

typedef enum { E2_IDLE = 0, E2_SAME, E2_MULTI } e2_mode_t;

static volatile bool    s_run;
static volatile u32     s_count[MAX_TASK];
static u8               s_level[MAX_TASK];
static ca_task_handle_t s_h[MAX_TASK];
static u32              s_n;
static u32              s_busy_cyc;
static u32              s_busy_us;
static u32              s_sleep_ms;
static u32              s_load;        /* 0 = busy */
static u32              s_t0_ms;
static e2_mode_t        s_mode;

static const char *const s_wname[] = {
    "w00","w01","w02","w03","w04","w05","w06","w07","w08","w09","w10","w11",
    "w12","w13","w14","w15","w16","w17","w18","w19","w20","w21","w22","w23",
    "w24","w25","w26","w27","w28","w29","w30","w31","w32","w33","w34","w35",
    "w36","w37","w38","w39","w40","w41","w42","w43","w44","w45","w46","w47",
};
#define E2_NAMES  (sizeof(s_wname) / sizeof(s_wname[0]))

/* Periodic: a round starts every E2_PERIOD_MS from the previous start, not
 * E2_PERIOD_MS after the end of the busy part. With a plain sleep the workers
 * locked in phase: round robin brings them to the end of their busy part
 * together, then all of them slept at once and the idle ran in the gap - 22 %
 * idle at a requested 200 % on the H773. A round that ends late does not sleep
 * and the next one starts from now (no catch-up burst). */
static u32 worker(void *arg) {
    const u32 i = (u32)(uintptr_t)arg;
    u32 next = ca_os_tick_ms();
    while (s_run) {
        const u32 t0 = ca_scheduler_get_runtime_total();
        while (s_run && (ca_scheduler_get_runtime_total() - t0) < s_busy_cyc) { }
        s_count[i]++;
        if (s_sleep_ms == 0U) continue;                 /* busy mode */
        next += E2_PERIOD_MS;
        const u32 now = ca_os_tick_ms();
        if ((i32)(next - now) > 0) {
            ca_os_sleep(next - now);
        } else {
            next = now;
        }
    }
    return 0;
}

/* ---- helpers ------------------------------------------------------------- */

static u32 live_tasks(void) {
    u32 n = 0U;
    for (u32 i = 0U; i < MAX_TASK; i++) {
        if (ca_scheduler_get_task_name(i) != NULL) n++;
    }
    return n;
}

/* Workers that fit: free task slots (one kept for the system) and stacks of
 * STACK_SMALL in the PSP arena, after keeping the STACK_LARGE of one 'cli_cmd'
 * (the task every CLI command runs in). Without that reserve, on the 48 KB
 * arena of the H773 the workers took it all and the next command ('get',
 * 'stop') ran in the CLI task after a SAFEGUARD line (seen in Renode 02/11). */
static u32 workers_that_fit(void) {
    const u32 live = live_tasks();
    u32 n = (live + 1U < MAX_TASK) ? (MAX_TASK - live - 1U) : 0U;
    const u32 free_b = ca_ccm_get_free();
    const u32 by_stack = (free_b > (u32)STACK_LARGE)
                       ? (free_b - (u32)STACK_LARGE) / (u32)STACK_SMALL : 0U;
    if (n > by_stack) n = by_stack;
    if (n > E2_NAMES) n = E2_NAMES;
    return n;
}

static const char *parse_u32(const char *p, u32 *out, bool *got) {
    u32 v = 0U;
    *got = false;
    while (*p >= '0' && *p <= '9') { v = v * 10U + (u32)(*p - '0'); p++; *got = true; }
    *out = v;
    return p;
}

/* [load%|busy]: sets s_load, s_busy_cyc, s_busy_us, s_sleep_ms for n workers */
static bool parse_load(const char *p, u32 n) {
    p = ca_skip_sp(p);
    u32 load = E2_LOAD_DEFAULT;
    if (ca_strncmp(p, "busy", 4) == 0) {
        load = 0U;
    } else if (*p != '\0') {
        bool got;
        (void)parse_u32(p, &load, &got);
        if (!got || load == 0U) return false;
    }
    s_load = load;
    const u32 period_us = E2_PERIOD_MS * 1000U;
    if (load == 0U) {
        s_busy_us = E2_BUSY_UNIT_US;
        s_sleep_ms = 0U;
    } else {
        u32 busy = (u32)(((u64)period_us * load) / (100U * (u64)n));
        if (busy > period_us) busy = period_us;
        if (busy == 0U) busy = 1U;
        s_busy_us = busy;
        s_sleep_ms = (period_us - busy) / 1000U;
    }
    s_busy_cyc = (u32)(((u64)ca_os_clock_get_freq() * s_busy_us) / 1000000U);
    return true;
}

static void start_workers(u32 n, e2_mode_t mode, u32 same_lvl) {
    for (u32 i = 0U; i < MAX_TASK; i++) { s_count[i] = 0U; s_h[i] = CA_TASK_HANDLE_NONE; }
    const u32 per = n / E2_LEVELS, rest = n % E2_LEVELS;
    u32 i = 0U;
    if (mode == E2_SAME) {
        for (; i < n; i++) s_level[i] = (u8)same_lvl;
    } else {
        for (u32 l = 0U; l < E2_LEVELS; l++) {
            const u32 k = per + ((l < rest) ? 1U : 0U);
            for (u32 j = 0U; j < k; j++) s_level[i++] = (u8)l;
        }
    }
    s_run = true;
    s_mode = mode;
    s_t0_ms = ca_os_tick_ms();
    s_n = 0U;
    for (i = 0U; i < n; i++) {
        s_h[i] = ca_os_job(s_wname[i], (u16)(1U << s_level[i]), worker, (void *)(uintptr_t)i,
                           STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
        if (s_h[i] == CA_TASK_HANDLE_NONE) break;
        s_n++;
    }
}

static void print_params(const char *what) {
    if (s_load == 0U) {
        CLI_PRINTF_N("%s: %u workers, busy (no sleep), round = %u us of CPU", what, s_n,
                     s_busy_us);
    } else {
        CLI_PRINTF_N("%s: %u workers, load %u%%, busy %u us of CPU every %u ms", what, s_n,
                     s_load, s_busy_us, E2_PERIOD_MS);
    }
}

/* CPU load over E2_WINDOW_MS, from the runtime of every task but the idle.
 * The idle's own runtime is not used: CYCCNT stops in WFI on some parts. */
static u32 cpu_load_permille(void) {
    static u32 rt0[MAX_TASK];
    static bool seen[MAX_TASK];
    ca_thread_context_t c;
    u32 slot;
    for (u32 i = 0U; i < MAX_TASK; i++) seen[i] = false;
    for (u32 t = 0U; t < MAX_TASK; t++) {
        if (ca_os_task_snapshot(t, &c, &slot) && slot < MAX_TASK &&
            ca_strncmp(c.name, "idle", 4) != 0) {
            rt0[slot] = c.runtime_total;
            seen[slot] = true;
        }
    }
    const u32 ms0 = ca_os_tick_ms();
    ca_os_sleep(E2_WINDOW_MS);
    const u32 dms = ca_os_tick_ms() - ms0;
    u64 busy = 0U;
    for (u32 t = 0U; t < MAX_TASK; t++) {
        if (ca_os_task_snapshot(t, &c, &slot) && slot < MAX_TASK && seen[slot] &&
            ca_strncmp(c.name, "idle", 4) != 0) {
            busy += (u32)(c.runtime_total - rt0[slot]);
        }
    }
    const u64 total = ((u64)ca_os_clock_get_freq() / 1000U) * dms;
    if (total == 0U) return 0U;
    u32 pm = (u32)((busy * 1000U) / total);
    return (pm > 1000U) ? 1000U : pm;
}

/* ---- commands ------------------------------------------------------------ */

static void cmd_same(const char *p) {
    if (s_mode != E2_IDLE) { CLI_PUT_N("same: workers running - 'stop' first"); return; }
    p = ca_skip_sp(p);
    u32 lvl;
    bool got;
    p = parse_u32(p, &lvl, &got);
    if (!got || lvl >= E2_LEVELS) { CLI_PUT_N("same: level 0..6"); return; }
    const u32 n = workers_that_fit();
    if (n == 0U || !parse_load(p, n)) { CLI_PUT_N("same: no room or bad load"); return; }
    start_workers(n, E2_SAME, lvl);
    print_params("same");
    CLI_PRINTF_N("same: all at CA_TH_LEVEL%u (weight %u)", lvl, 1U << lvl);
}

static void cmd_multi(const char *p) {
    if (s_mode != E2_IDLE) { CLI_PUT_N("multi: workers running - 'stop' first"); return; }
    const u32 n = workers_that_fit();
    if (n == 0U || !parse_load(p, n)) { CLI_PUT_N("multi: no room or bad load"); return; }
    start_workers(n, E2_MULTI, 0U);
    print_params("multi");
}

static void cmd_get(void) {
    if (s_mode == E2_IDLE) { CLI_PUT_N("get: no workers - 'same <lvl>' or 'multi'"); return; }
    const u32 pm = cpu_load_permille();
    const u32 el = ca_os_tick_ms() - s_t0_ms;
    print_params((s_mode == E2_SAME) ? "same" : "multi");
    /* The rest is not idle only: the scheduler (PendSV) and the interrupts are
     * booked to no task either - about 6 % with 14 busy workers on the H773. */
    CLI_PRINTF_N("elapsed %u.%u s, CPU to tasks %u.%u%%, rest %u.%u%% (idle, scheduler, IRQs)",
                 el / 1000U, (el % 1000U) / 100U, pm / 10U, pm % 10U, (1000U - pm) / 10U,
                 (1000U - pm) % 10U);
    u32 counts[MAX_TASK];
    u64 sum = 0U;
    for (u32 i = 0U; i < s_n; i++) { counts[i] = s_count[i]; sum += counts[i]; }
    CLI_PUT_N(" task  lvl   count   share");
    for (u32 i = 0U; i < s_n; i++) {
        const u32 sh = (sum != 0U) ? (u32)(((u64)counts[i] * 1000U) / sum) : 0U;
        CLI_PRINTF_N(" %s   L%u  %7u  %2u.%u%%", s_wname[i], s_level[i], counts[i], sh / 10U,
                     sh % 10U);
    }
    if (s_mode == E2_SAME) {
        u32 mn = 0xFFFFFFFFU, mx = 0U;
        for (u32 i = 0U; i < s_n; i++) {
            if (counts[i] < mn) mn = counts[i];
            if (counts[i] > mx) mx = counts[i];
        }
        const u32 avg = (s_n != 0U) ? (u32)(sum / s_n) : 0U;
        const u32 spread = (avg != 0U) ? (u32)(((u64)(mx - mn) * 1000U) / avg) : 0U;
        CLI_PRINTF_N("round robin: min %u max %u avg %u, spread (max-min)/avg %u.%u%%", mn, mx,
                     avg, spread / 10U, spread % 10U);
    } else {
        u32 avg[E2_LEVELS] = {0U}, cnt[E2_LEVELS] = {0U};
        u64 lsum[E2_LEVELS] = {0U};
        for (u32 i = 0U; i < s_n; i++) { lsum[s_level[i]] += counts[i]; cnt[s_level[i]]++; }
        for (u32 l = 0U; l < E2_LEVELS; l++) avg[l] = cnt[l] ? (u32)(lsum[l] / cnt[l]) : 0U;
        CLI_PUT_N(" level tasks   avg   measured  weights   (ratio to L0)");
        for (u32 l = 0U; l < E2_LEVELS; l++) {
            const u32 r = (avg[0] != 0U) ? (u32)(((u64)avg[l] * 100U) / avg[0]) : 0U;
            CLI_PRINTF_N("  L%u    %2u  %7u  %4u.%02u   %4u", l, cnt[l], avg[l], r / 100U, r % 100U,
                         1U << l);
        }
    }
}

static void cmd_stop(void) {
    if (s_mode == E2_IDLE) { CLI_PUT_N("stop: no workers"); return; }
    s_run = false;
    u32 left = s_n;
    for (u32 t = 0U; t < E2_STOP_MS && left != 0U; t += 10U) {
        ca_os_sleep(10);
        left = 0U;
        for (u32 i = 0U; i < s_n; i++) {
            if (ca_os_thid_from_handle(s_h[i]) != (u32)-1) left++;
        }
    }
    CLI_PRINTF_N("stop: %u workers ended%s", s_n - left, left ? " (some still running!)" : "");
    if (left == 0U) s_mode = E2_IDLE;
}

/* one entry per command word; each gets the line without its word */
static bool same_cmd(c_str *args)
{
    const char *p = (*args)->data;
    cmd_same(p);
    return true;
}

static bool multi_cmd(c_str *args)
{
    const char *p = (*args)->data;
    cmd_multi(p);
    return true;
}

static bool get_cmd(c_str *args)
{
    (void)args;
    cmd_get();
    return true;
}

static bool stop_cmd(c_str *args)
{
    (void)args;
    cmd_stop();
    return true;
}

static const ca_cli_cmd_t s_user_cmds[] = {
    { "same", "same      : <lvl 0..6> [load%|busy] workers all at one level (round robin)", { same_cmd, STACK_MEDIUM } },
    { "multi", "multi     : [load%|busy] workers over the 7 levels (CFS)", { multi_cmd, STACK_MEDIUM } },
    { "get", "get       : counters, CPU load, ratios", { get_cmd, STACK_MEDIUM } },
    { "stop", "stop      : end the workers", { stop_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_user_table = {
    "user", s_user_cmds, (u16)(sizeof(s_user_cmds) / sizeof(s_user_cmds[0])), NULL
};

static u32 task_main(void *arg) {
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("example_2: scheduler demo, idle - 'same <lvl> [load%|busy]', 'multi', "
                    "'get', 'stop'");
    return 0;
}

void ca_os_configure(void) {
    /* UART DBG or ITM ALREADY SET ON SYSTEM IF ENABLED! */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    ca_cli_init(4096);
    ca_cli_set_sender(STDOUT);
    ca_cli_create_thread();
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}

void ca_os_main(void) {
    s_mode = E2_IDLE;
    ca_os_job("example_2", CA_TH_LEVEL3, task_main, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_ASSERT);
    ca_cli_register_table(&s_user_table);
    ca_os_run(true);
}

#else  /* no CLI */

PRIVATE_STRING(m2a, "=== Example 2 ===");
PRIVATE_STRING(m2b, "ENABLE_CLI disabled: the scheduler demo is driven from the CLI");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m2a);
    PUT_N(&priv_m2b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
