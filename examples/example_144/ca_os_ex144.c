// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex144.c
 * @brief          : Example 144 — console owner: a task takes the console
 ******************************************************************************
 * @details
 * The console input normally goes to the CLI, a line at a time. A task can
 * take it (ca_console_take) and read it a character at a time, with no echo
 * and no line editing, then give it back (ca_console_give). This is what a
 * REPL or an editor needs. Each command starts a job and returns at once, so
 * no cli_cmd slot stays busy while the task owns the console.
 *
 *   con echo       character echo; Ctrl-C is seen by the ISR filter (prints
 *                  ^C at once), Ctrl-D ends and gives the console back.
 *   con count N    receives N bytes as fast as they come (paste test) and
 *                  prints "CON COUNT END got=.. fnv=.. dropped=.." with the
 *                  FNV-1a hash of what arrived; 3 s of silence ends it early.
 *                  Ends with "CON COUNT PASS" or "CON COUNT FAIL".
 *   con die        a task takes the console and ends without giving it back:
 *                  the next byte typed must reach the CLI again (reclaimed).
 *   con stat       console counters.
 ******************************************************************************
 */

#include "caos.h"

#if ENABLE_CLI

#define CTRL_C      0x03U
#define CTRL_D      0x04U
#define IDLE_MS     3000U
#define FNV_SEED    2166136261U
#define FNV_PRIME   16777619U

static volatile u32 s_ctrlc;

/* ISR context: Ctrl-C is an event, not a byte waiting behind the task */
static bool echo_filter(u8 byte)
{
    if (byte != CTRL_C) return false;
    s_ctrlc++;
    (void)ca_stdout_echo("^C", 2U);
    return true;
}

static u32 echo_task(void *arg)
{
    (void)arg;
    u32 n = 0U;
    s_ctrlc = 0U;
    if (!ca_console_take(echo_filter)) {
        CLI_PUT_N("CON ECHO: console busy");
        return 0;
    }
    CLI_PUT_N("CON ECHO START (Ctrl-D ends)");
    for (;;) {
        const i32 c = ca_console_getc(CA_WAIT_FOREVER);
        if (c < 0) break;
        if ((u32)c == CTRL_D) break;
        n++;
        if (c == '\r') {
            (void)ca_stdout_echo("\r\n", 2U);
        } else {
            const char ch = (char)c;
            (void)ca_stdout_echo(&ch, 1U);
        }
    }
    (void)ca_console_give();
    CLI_PRINTF_N("\r\nCON ECHO END bytes=%u ctrlc=%u", n, s_ctrlc);
    return 0;
}

static u32 count_task(void *arg)
{
    const u32 want = (u32)arg;
    ca_console_stats_t st0, st1;
    u32 got = 0U, h = FNV_SEED;

    ca_console_stats(&st0);
    if (!ca_console_take(NULL)) {
        CLI_PUT_N("CON COUNT: console busy");
        return 0;
    }
    CLI_PRINTF_N("CON COUNT READY n=%u", want);
    while (got < want) {
        const i32 c = ca_console_getc(IDLE_MS);
        if (c < 0) break;
        h = (h ^ (u32)c) * FNV_PRIME;
        got++;
    }
    (void)ca_console_give();
    ca_console_stats(&st1);
    const u32 dropped = st1.dropped - st0.dropped;
    CLI_PRINTF_N("CON COUNT END got=%u fnv=%u dropped=%u", got, h, dropped);
    CLI_PUT_N((got == want && dropped == 0U) ? "CON COUNT PASS" : "CON COUNT FAIL");
    return 0;
}

static u32 die_task(void *arg)
{
    (void)arg;
    if (!ca_console_take(NULL)) {
        CLI_PUT_N("CON DIE: console busy");
        return 0;
    }
    CLI_PUT_N("CON DIE: owner ends without giving the console back");
    return 0;                       /* the next byte reclaims the console */
}

static void con_stat(void)
{
    ca_console_stats_t st;
    ca_console_stats(&st);
    CLI_PRINTF_N("CON STAT to_task=%u dropped=%u filtered=%u reclaimed=%u ring=%u",
                 st.to_task, st.dropped, st.filtered, st.reclaimed, (u32)CONSOLE_RX_BUF_SIZE);
}

static bool start(const char *name, ca_task_fn_t fn, void *arg)
{
    const ca_task_handle_t h = ca_os_job(name, CA_TH_LEVEL3, fn, arg, STACK_MEDIUM,
                                         CA_JOB_STD, CA_FAULT_ASSERT);
    if (h == CA_TASK_HANDLE_NONE) CLI_PRINTF_N("CON: cannot start %s", name);
    return true;
}

/* con — the line arrives without the command word */
static bool con_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "echo", 4) == 0)  return start("con_echo", echo_task, NULL);
    if (ca_strncmp(p, "die", 3) == 0)   return start("con_die", die_task, NULL);
    if (ca_strncmp(p, "stat", 4) == 0)  { con_stat(); return true; }
    if (ca_strncmp(p, "count ", 6) == 0) {
        const i32 n = ca_strtol(p + 6, 10U);
        if (n <= 0) return false;
        return start("con_count", count_task, (void *)(u32)n);
    }
    return false;                   /* cli prints the help line */
}

static const ca_cli_cmd_t s_con_cmds[] = {
    { "con", "con       : echo | count N | die | stat", { con_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_con_table = {
    "con", s_con_cmds, (u16)(sizeof(s_con_cmds) / sizeof(s_con_cmds[0])), NULL
};

void ca_os_configure(void)
{
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register_table(&s_con_table);
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else

PRIVATE_STRING(m144a, "=== Example 144 ===");
PRIVATE_STRING(m144b, "ex144: ENABLE_CLI disabled");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m144a);
    PUT_N(&priv_m144b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
