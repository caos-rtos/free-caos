// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex108.c
 * @brief          : Example 108 — the heap: who owns a block, who frees it
 ******************************************************************************
 * @details
 * The caOS heap (memory.h) ties every block to the task that allocated it:
 * when the task ends, the kernel frees whatever it left behind. A block that
 * must outlive its creator is allocated SHARED, and the task that receives
 * it can ADOPT it. Four cases, each one a verdict:
 *
 *   heap basic    ca_memory_allocate() takes the bytes from the heap and
 *                 charges them to the calling task (ca_memory_get_task_usage);
 *                 ca_memory_free(p, &size, true) wipes the payload and gives
 *                 everything back.
 *   heap owned    a task allocates three blocks and returns WITHOUT freeing
 *                 them: the kernel frees them at its exit ('Memory cleanup').
 *   heap shared   a producer allocates a block with ca_memory_allocate_shared()
 *                 and returns: the block survives, with its content. A
 *                 consumer adopts it (ca_memory_adopt) and returns without
 *                 freeing it: now it is the consumer's, and goes with it.
 *   heap check    ca_memory_check(): one walk of the whole block chain, the
 *                 counters the 'free' command prints.
 *   heap all      the four in a row: "RUN RESULT : OK  (4/4)"
 *
 * A CLI command runs in a temporary task (cli_cmd) too: what a command
 * allocates with ca_memory_allocate() is freed when the command returns.
 * Never keep a pointer to it in a static for the next command.
 ******************************************************************************
 */

#include "caos.h"

#define BASIC_SIZE    300U
#define SHARED_SIZE   128U
#define TASK_WAIT_MS  2000U

static const u32 s_owned_size[3] = { 100U, 200U, 300U };
static const char s_text[] = "caOS shared block";

/* Results the worker tasks leave for the command that started them */
static volatile u32   g_owned_ok;      /* blocks the owner task obtained */
static volatile u32   g_owned_usage;   /* its ca_memory_get_task_usage() */
static volatile u8   *g_shared;        /* the block handed over */
static volatile bool  g_adopt_ok;
static volatile u32   g_adopt_usage;

static u16 self_slot(void)
{
    return (u16)ca_os_task_slot(ca_scheduler_get_current_context());
}

/* The handle stops resolving once the task is gone (and its blocks freed) */
static bool wait_gone(ca_task_handle_t h)
{
    for (u32 t = 0; t < TASK_WAIT_MS; t += 10U) {
        if (ca_os_thid_from_handle(h) == (u32)-1) {
            ca_os_sleep(20);
            return true;
        }
        ca_os_sleep(10);
    }
    return false;
}

static ca_memory_check_t heap_now(void)
{
    ca_memory_check_t c;
    (void)ca_memory_check(&c);
    return c;
}

/* ---------------------------------------------------------------- basic */

static bool heap_basic(void)
{
    const u16 slot = self_slot();
    const u32 a0 = ca_memory_get_avail_memory();
    const u32 u0 = ca_memory_get_task_usage(slot);

    u8 *p = (u8 *)ca_memory_allocate(BASIC_SIZE);
    if (p == NULL) {
        CLI_PUT_N("  basic : FAIL (ca_memory_allocate returned NULL)");
        return false;
    }
    const u32 a1 = ca_memory_get_avail_memory();
    const u32 u1 = ca_memory_get_task_usage(slot);
    for (u32 i = 0; i < BASIC_SIZE; i++) p[i] = 0xA5U;

    u32 freed = 0;
    const bool fr = ca_memory_free(p, &freed, true);
    /* Read right away, before anything else can reuse the block: the wipe
     * zeroed the payload (nothing else to see in it, it is FREE now). */
    u32 nonzero = 0;
    for (u32 i = 0; i < BASIC_SIZE; i++) nonzero += (p[i] != 0U);
    const u32 a2 = ca_memory_get_avail_memory();
    const u32 u2 = ca_memory_get_task_usage(slot);

    const bool ok = fr && a1 + BASIC_SIZE <= a0 && u1 >= u0 + BASIC_SIZE &&
                    freed >= BASIC_SIZE && nonzero == 0U && a2 == a0 && u2 == u0;
    CLI_PRINTF_N("  basic : avail %u -> %u -> %u, task usage %u -> %u -> %u",
                 a0, a1, a2, u0, u1, u2);
    CLI_PRINTF_N("          freed %u B, %u payload bytes not wiped",
                 freed, nonzero);
    CLI_PRINTF_N("  basic : %s", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---------------------------------------------------------------- owned */

static u32 owner_task(void *arg)
{
    (void)arg;
    u32 n = 0;
    for (u32 i = 0; i < 3U; i++) {
        u8 *b = (u8 *)ca_memory_allocate(s_owned_size[i]);
        if (b) { b[0] = (u8)i; n++; }
    }
    g_owned_usage = ca_memory_get_task_usage(self_slot());
    g_owned_ok = n;
    ca_stdout_printf_n("[EX108] owner: %u blocks, %u B charged to me, returning without free",
                       n, (u32)g_owned_usage);
    return 0;                       /* the kernel frees the three blocks */
}

static bool heap_owned(void)
{
    const ca_memory_check_t c0 = heap_now();
    g_owned_ok = 0;
    g_owned_usage = 0;
    const ca_task_handle_t h = ca_os_job("e108_own", CA_TH_LEVEL3, owner_task, NULL,
                                         STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    const bool gone = (h != CA_TASK_HANDLE_NONE) && wait_gone(h);
    const ca_memory_check_t c1 = heap_now();

    const bool ok = gone && g_owned_ok == 3U &&
                    g_owned_usage >= s_owned_size[0] + s_owned_size[1] + s_owned_size[2] &&
                    c1.avail == c0.avail && c1.busy == c0.busy;
    CLI_PRINTF_N("  owned : task %s, avail %u -> %u, busy blocks %u -> %u",
                 gone ? "ended" : "STILL RUNNING", c0.avail, c1.avail, c0.busy, c1.busy);
    CLI_PRINTF_N("  owned : %s", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---------------------------------------------------------------- shared */

static u32 producer_task(void *arg)
{
    (void)arg;
    u8 *b = (u8 *)ca_memory_allocate_shared(SHARED_SIZE);
    if (b) {
        for (u32 i = 0; i < sizeof(s_text); i++) b[i] = (u8)s_text[i];
    }
    g_shared = b;
    ca_stdout_put_n("[EX108] producer: shared block filled, returning without free");
    return 0;                       /* a shared block is NOT freed here */
}

static u32 consumer_task(void *arg)
{
    (void)arg;
    g_adopt_ok = ca_memory_adopt((void *)g_shared);
    g_adopt_usage = ca_memory_get_task_usage(self_slot());
    ca_stdout_printf_n("[EX108] consumer: adopted %s, %u B charged to me, returning without free",
                       g_adopt_ok ? "yes" : "NO", (u32)g_adopt_usage);
    return 0;                       /* now it is mine: freed with me */
}

static bool heap_shared(void)
{
    const ca_memory_check_t c0 = heap_now();
    g_shared = NULL;
    g_adopt_ok = false;
    g_adopt_usage = 0;

    ca_task_handle_t h = ca_os_job("e108_prod", CA_TH_LEVEL3, producer_task, NULL,
                                   STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    const bool p_gone = (h != CA_TASK_HANDLE_NONE) && wait_gone(h);
    const ca_memory_check_t c1 = heap_now();
    bool text_ok = (g_shared != NULL);
    for (u32 i = 0; text_ok && i < sizeof(s_text); i++) text_ok = (g_shared[i] == (u8)s_text[i]);
    const bool survived = p_gone && text_ok && c1.shared == c0.shared + 1U &&
                          c1.shared_bytes >= c0.shared_bytes + SHARED_SIZE;

    h = ca_os_job("e108_cons", CA_TH_LEVEL3, consumer_task, NULL,
                  STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    const bool c_gone = (h != CA_TASK_HANDLE_NONE) && wait_gone(h);
    const ca_memory_check_t c2 = heap_now();
    const bool released = c_gone && g_adopt_ok && g_adopt_usage >= SHARED_SIZE &&
                          c2.shared == c0.shared && c2.avail == c0.avail;

    CLI_PRINTF_N("  shared: after the producer: shared blocks %u -> %u, content %s",
                 c0.shared, c1.shared, text_ok ? "intact" : "LOST");
    CLI_PRINTF_N("  shared: after the consumer: shared blocks %u, avail %u -> %u",
                 c2.shared, c0.avail, c2.avail);
    const bool ok = survived && released;
    CLI_PRINTF_N("  shared: %s", ok ? "PASS" : "FAIL");
    return ok;
}

/* ---------------------------------------------------------------- check */

static bool heap_check(void)
{
    ca_memory_check_t c;
    const bool ok = ca_memory_check(&c);
    CLI_PRINTF_N("  check : %u blocks: %u busy (%u B), %u free (%u B), largest free %u B",
                 c.blocks, c.busy, c.busy_bytes, c.free, c.free_bytes, c.largest_free);
    CLI_PRINTF_N("          shared %u (%u B), avail %u expected %u, chain %s, canary %s",
                 c.shared, c.shared_bytes, c.avail, c.avail_expect,
                 c.chain_ok ? "ok" : "BROKEN", c.canary_ok ? "ok" : "BROKEN");
    if (!ok && c.bad_what) {
        CLI_PRINTF_N("          first defect: %s at 0x%08X", c.bad_what, c.bad_addr);
    }
    CLI_PRINTF_N("  check : %s", ok ? "PASS" : "FAIL");
    return ok;
}

static void heap_all(void)
{
    u32 n = 0;
    n += heap_basic()  ? 1U : 0U;
    n += heap_owned()  ? 1U : 0U;
    n += heap_shared() ? 1U : 0U;
    n += heap_check()  ? 1U : 0U;
    /* The line the bench reads */
    CLI_PRINTF_N("  RUN RESULT : %s  (%u/4)", (n == 4U) ? "OK" : "FAIL", n);
}

#if ENABLE_CLI
/* heap — the line arrives without the command word */
static bool heap_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "basic", 5) == 0)  { (void)heap_basic();  return true; }
    if (ca_strncmp(p, "owned", 5) == 0)  { (void)heap_owned();  return true; }
    if (ca_strncmp(p, "shared", 6) == 0) { (void)heap_shared(); return true; }
    if (ca_strncmp(p, "check", 5) == 0)  { (void)heap_check();  return true; }
    if (ca_strncmp(p, "all", 3) == 0)    { heap_all();          return true; }
    return false;                   /* cli prints the help line */
}
static const ca_cli_cmd_t s_heap_cmds[] = {
    { "heap", "heap      : basic | owned | shared | check | all", { heap_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_heap_table = {
    "heap", s_heap_cmds, (u16)(sizeof(s_heap_cmds) / sizeof(s_heap_cmds[0])), NULL
};
#endif

PRIVATE_STRING(banner, "ex108: heap ownership demo, idle - type 'heap all'");

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
        ca_cli_register_table(&s_heap_table);
    )
    PUT_N(&priv_banner);
}

void ca_os_main(void)
{
    ca_os_run(true);
}
