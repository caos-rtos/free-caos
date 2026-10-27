// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 88 — Thread-Metric on caOS
 *
 * Thread-Metric (external/thread-metric-benchmark, a git submodule) is the
 * RTOS throughput suite that ThreadX and FreeRTOS publish their numbers with:
 * eight tests, each counting how many times a primitive round-trips in a
 * fixed interval. This example compiles the eight UPSTREAM test sources as
 * they are and supplies the porting layer (tm_api.h) on caOS primitives, so
 * the numbers compare with the published ones on the same hardware.
 *
 *   basic        one thread, pure computation (the baseline)
 *   coop         5 threads of equal priority, relinquish round robin
 *   preempt      5 threads of rising priority, resume/suspend chain
 *   irq          a thread calls the ISR body in line, it posts a semaphore
 *   irq_preempt  a real IRQ resumes a higher priority thread
 *   message      one thread, queue send + receive of 16 bytes
 *   sync         one thread, semaphore get + put
 *   memory       one thread, 128 byte block allocate + free
 *
 *   e88 run [test|all] [sec]   default all, 10 s each (upstream uses 30)
 *   e88 info
 *
 * Every test prints the upstream report ("Time Period Total: N", and an
 * "ERROR: ..." line when its own sanity check fails), then one verdict line
 * from this example: e88 <test> : total=.. per_sec=.. errors=..  PASS|FAIL.
 * coop and preempt check an ORDER (counters within 1 of the average) that
 * only strict round robin / strict priority give: on caOS their ERROR is
 * reported as order=N, and the verdict asks that no thread starved (min).
 * This example measures caOS; it is not set up as a comparison with other
 * kernels.
 *
 * Porting choices, where caOS differs from a strict-priority RTOS:
 *   - priority: Thread-Metric 1 (highest)..31 maps onto the seven caOS
 *     levels, 2 -> LEVEL6 ... 10 -> LEVEL0. caOS levels are WEIGHTS of a
 *     fair scheduler (CFS-like), not strict priorities: a resumed thread of
 *     higher priority does not preempt by rule. The port yields after such
 *     a resume (and pends PendSV from the ISR), which is what an RTOS port
 *     does on its own; who runs next is still the scheduler's choice.
 *   - threads are created suspended (as TX_DONT_START): the entry parks
 *     itself until tm_thread_resume() lets it go.
 *   - tm_cause_interrupt(): TIM3 with its counter stopped, update event
 *     generated on demand (EGR = UG): a real IRQ through the caOS timer
 *     driver. tm_cause_interrupt_sync(): the handler in line, IRQs masked.
 *   - queue: static queue, 10 x 16 bytes, no wait (as the FreeRTOS port).
 *     semaphore: counting, starts at 1, get does not wait. memory pool:
 *     caOS block pool, 16 blocks of 128 bytes.
 */

#include "caos.h"

#if EN_TIMER && EN_BLOCK_POOL && ENABLE_CLI && (MAX_TASK >= 16)

#include <stdarg.h>
#include "tm_api.h"

/* ============================================================================
 * The upstream tests, compiled in place. Each defines tm_main(): renamed so
 * the eight live in one image.
 * ========================================================================== */

#define tm_main tm88_main_basic
#include "../../external/thread-metric-benchmark/src/basic_processing.c"
#undef tm_main
#define tm_main tm88_main_coop
#include "../../external/thread-metric-benchmark/src/cooperative_scheduling.c"
#undef tm_main
#define tm_main tm88_main_preempt
#include "../../external/thread-metric-benchmark/src/preemptive_scheduling.c"
#undef tm_main
#define tm_main tm88_main_irq
#include "../../external/thread-metric-benchmark/src/interrupt_processing.c"
#undef tm_main
#define tm_main tm88_main_irq_preempt
#include "../../external/thread-metric-benchmark/src/interrupt_preemption_processing.c"
#undef tm_main
#define tm_main tm88_main_message
#include "../../external/thread-metric-benchmark/src/message_processing.c"
#undef tm_main
#define tm_main tm88_main_sync
#include "../../external/thread-metric-benchmark/src/synchronization_processing.c"
#undef tm_main
#define tm_main tm88_main_memory
#include "../../external/thread-metric-benchmark/src/memory_allocation.c"
#undef tm_main

/* ============================================================================
 * Run state
 * ========================================================================== */

#define TM88_THREADS     10
#define TM88_TIM         3U
#define TM88_SEC_DEFAULT 10U
#define TM88_QUEUE_DEPTH 10U
#define TM88_BLOCK_SIZE  128U
#define TM88_POOL_SIZE   2048U

static volatile ca_task_handle_t s_h[TM88_THREADS];
static volatile u8   s_go[TM88_THREADS];
static void        (*s_entry[TM88_THREADS])(void);
static void        (*volatile s_isr)(void);     /* handler of the running test */

static volatile bool s_done;          /* report finished (or init failed) */
static volatile bool s_failed;        /* tm_check_fail() */
static volatile u32  s_errors;        /* "ERROR" lines of the upstream report */
static volatile u32  s_total;         /* last "Time Period Total" */
static volatile bool s_have_total;
static volatile bool s_busy;
static bool          s_tim_ready;

static CAQUEUE_STATIC_DEFINE(s_tmq, TM88_QUEUE_DEPTH, 4U * sizeof(unsigned long));
static ca_semaphore_t   s_tmsem;
static ca_block_pool_t   s_tmpool;
static u8            s_tmpool_buf[TM88_POOL_SIZE] __attribute__((aligned(8)));

/* ============================================================================
 * Report helpers (upstream tm_report.c calls exit() and getenv(): not here)
 * ========================================================================== */

int tm_test_duration = (int)TM88_SEC_DEFAULT;
int tm_test_cycles   = 1;

void tm_report_init(void) { }
void tm_report_init_argv(int argc, char **argv) { (void)argc; (void)argv; }

static char s_line[128];
static u32  s_line_len;

/* Lines, not characters, reach the console; the report is also read here. */
void tm_putchar(int c) {
    if (c == '\r') return;
    if (c != '\n') {
        if (s_line_len < sizeof(s_line) - 1U) s_line[s_line_len++] = (char)c;
        return;
    }
    s_line[s_line_len] = '\0';
    if (s_line_len > 0U) {
        if (ca_strncmp(s_line, "ERROR", 5) == 0) s_errors++;
        static const char tot[] = "Time Period Total:";
        if (ca_strncmp(s_line, tot, sizeof(tot) - 1U) == 0) {
            const char *p = ca_skip_sp(s_line + sizeof(tot) - 1U);
            s_total = (u32)ca_strtol(p, 10);
            s_have_total = true;
        }
        ca_stdout_put_n(s_line);
    }
    s_line_len = 0U;
}

static void tm88_put_ul(unsigned long v) {
    char b[12];
    u32 i = 0U;
    do { b[i++] = (char)('0' + (v % 10UL)); v /= 10UL; } while (v != 0UL && i < sizeof(b));
    while (i > 0U) tm_putchar(b[--i]);
}

void tm_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') { tm_putchar(*fmt); continue; }
        fmt++;
        if (*fmt == 'l' && fmt[1] == 'u') {
            fmt++;
            tm88_put_ul(va_arg(ap, unsigned long));
        } else if (*fmt == 'd') {
            const int v = va_arg(ap, int);
            if (v < 0) { tm_putchar('-'); tm88_put_ul((unsigned long)(-(long)v)); }
            else tm88_put_ul((unsigned long)v);
        } else if (*fmt == 's') {
            for (const char *s = va_arg(ap, const char *); *s != '\0'; s++) tm_putchar(*s);
        } else if (*fmt == '\0') {
            break;
        } else {
            tm_putchar(*fmt);
        }
    }
    va_end(ap);
}

/* The report thread ends here after its one cycle: park until removed. */
void tm_report_finish(void) {
    s_done = true;
    while (1) ca_os_sleep(1000);
}

void tm_check_fail(const char *msg) {
    tm_printf("%s", msg);
    s_failed = true;
    s_done = true;
    while (1) ca_os_sleep(1000);
}

/* ============================================================================
 * Porting layer: threads
 * ========================================================================== */

static const char *const s_names[TM88_THREADS] = {
    "tm0", "tm1", "tm2", "tm3", "tm4", "tm5", "tm6", "tm7", "tm8", "tm9"
};

/* Thread-Metric 1 = highest .. 31 = lowest, onto the caOS weights */
static u16 tm88_level(int p) {
    if (p <= 2) return CA_TH_LEVEL6;
    if (p == 3) return CA_TH_LEVEL5;
    if (p <= 6) return CA_TH_LEVEL4;
    if (p == 7) return CA_TH_LEVEL3;
    if (p == 8) return CA_TH_LEVEL2;
    if (p == 9) return CA_TH_LEVEL1;
    return CA_TH_LEVEL0;
}

/* Created suspended: park until resumed. The check and the park are one
 * masked stretch, so a resume cannot fall between them and be lost. */
static u32 tm88_trampoline(void *arg) {
    const u32 id = (u32)(uintptr_t)arg;
    const u32 pm = __get_PRIMASK();
    __disable_irq();
    if (s_go[id] == 0U) (void)ca_os_suspend_handle(ca_os_self_handle());
    __set_PRIMASK(pm);
    ca_os_yield();
    s_entry[id]();
    while (1) ca_os_sleep(1000);        /* an entry that gave up (error) */
    return 0;
}

void tm_initialize(void (*test_initialization_function)(void)) {
    /* The scheduler already runs: just let the test create its threads. */
    test_initialization_function();
}

int tm_thread_create(int thread_id, int priority, void (*entry_function)(void)) {
    if (thread_id < 0 || thread_id >= TM88_THREADS || priority < 1 || priority > 31)
        return TM_ERROR;
    s_entry[thread_id] = entry_function;
    s_go[thread_id] = 0U;
    const ca_task_handle_t h = ca_os_job(s_names[thread_id], tm88_level(priority),
                                         tm88_trampoline, (void *)(uintptr_t)((u32)thread_id), STACK_SMALL,
                                         CA_JOB_STD, CA_FAULT_ASSERT);
    s_h[thread_id] = h;
    return (h == CA_TASK_HANDLE_NONE) ? TM_ERROR : TM_SUCCESS;
}

int tm_thread_resume(int thread_id) {
    if (thread_id < 0 || thread_id >= TM88_THREADS) return TM_ERROR;
    const u32 pm = __get_PRIMASK();
    __disable_irq();
    s_go[thread_id] = 1U;
    (void)ca_os_resume_handle(s_h[thread_id]);
    __set_PRIMASK(pm);

    /* A strict-priority RTOS switches here when the resumed thread outranks
     * the caller; caOS only reschedules. */
    if (__get_IPSR() != 0U) {
        SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
    } else {
        const ca_thread_context_t *cur = ca_scheduler_get_current_context();
        const ca_thread_context_t *tgt = ca_os_context_from_slot(s_h[thread_id] & 0xFFFFU);
        if (cur != NULL && tgt != NULL && tgt->priority > cur->priority) ca_os_yield();
    }
    return TM_SUCCESS;
}

int tm_thread_suspend(int thread_id) {
    if (thread_id < 0 || thread_id >= TM88_THREADS) return TM_ERROR;
    const bool self = (s_h[thread_id] == ca_os_self_handle());
    (void)ca_os_suspend_handle(s_h[thread_id]);
    if (self) ca_os_yield();
    return TM_SUCCESS;
}

void tm_thread_relinquish(void) {
    ca_os_yield();
}

void tm_thread_sleep(int seconds) {
    ca_os_sleep((u32)seconds * 1000U);
}

/* ============================================================================
 * Porting layer: queue, semaphore, memory pool
 * ========================================================================== */

int tm_queue_create(int queue_id) {
    if (queue_id != 0) return TM_ERROR;
    ca_queue_static_reset(&s_tmq.q);
    return TM_SUCCESS;
}

int tm_queue_send(int queue_id, unsigned long *message_ptr) {
    if (queue_id != 0) return TM_ERROR;
    return ca_queue_static_trysend(&s_tmq.q, message_ptr) ? TM_SUCCESS : TM_ERROR;
}

int tm_queue_receive(int queue_id, unsigned long *message_ptr) {
    if (queue_id != 0) return TM_ERROR;
    return ca_queue_static_tryreceive(&s_tmq.q, message_ptr) ? TM_SUCCESS : TM_ERROR;
}

int tm_semaphore_create(int semaphore_id) {
    if (semaphore_id != 0) return TM_ERROR;
    ca_sem_init(&s_tmsem, "TM88SEM", 1U, 0U);     /* starts available, no max */
    return TM_SUCCESS;
}

int tm_semaphore_get(int semaphore_id) {
    if (semaphore_id != 0) return TM_ERROR;
    return ca_sem_trywait(&s_tmsem) ? TM_SUCCESS : TM_ERROR;
}

int tm_semaphore_put(int semaphore_id) {
    if (semaphore_id != 0) return TM_ERROR;
    ca_sem_post(&s_tmsem);
    return TM_SUCCESS;
}

int tm_memory_pool_create(int pool_id) {
    if (pool_id != 0) return TM_ERROR;
    ca_block_pool_init(&s_tmpool, s_tmpool_buf, TM88_POOL_SIZE, TM88_BLOCK_SIZE);
    return TM_SUCCESS;
}

int tm_memory_pool_allocate(int pool_id, unsigned char **memory_ptr) {
    if (pool_id != 0 || memory_ptr == NULL) return TM_ERROR;
    unsigned char *b = (unsigned char *)ca_block_alloc(&s_tmpool);
    if (b == NULL) return TM_ERROR;
    *memory_ptr = b;
    return TM_SUCCESS;
}

int tm_memory_pool_deallocate(int pool_id, unsigned char *memory_ptr) {
    if (pool_id != 0) return TM_ERROR;
    ca_block_free(&s_tmpool, memory_ptr);
    return TM_SUCCESS;
}

/* ============================================================================
 * Porting layer: interrupts
 * ========================================================================== */

static volatile u32 s_irq_taken;         /* TIM3 callbacks run so far */

static void tm88_tim_cb(void) {
    void (*isr)(void) = s_isr;
    s_irq_taken++;
    if (isr != NULL) isr();
}

/* A real IRQ: update event on TIM3, whose counter never runs.
 *
 * DSB+ISB only drain the write: UIF still has to cross the APB bridge and
 * reach the NVIC. On F446 Release (APB1 at 45 MHz) the caller got through its
 * next UG before the first was taken, the two merged into one interrupt and
 * irq_preempt failed with thread_1 = 2 x handler + 1. So wait for the handler
 * itself; the bound only keeps a dead timer from hanging the thread. */
void tm_cause_interrupt(void) {
    const u32 before = s_irq_taken;
    TIM3->EGR = TIM_EGR_UG;
    __DSB();
    __ISB();
    for (u32 guard = 0U; s_irq_taken == before && guard < 1000U; guard++) { }
}

/* The handler body in line, IRQs masked: no trap, no scheduler round trip. */
void tm_cause_interrupt_sync(void) {
    void (*isr)(void) = s_isr;
    const u32 pm = __get_PRIMASK();
    __disable_irq();
    if (isr != NULL) isr();
    __set_PRIMASK(pm);
}

/* ============================================================================
 * The tests
 * ========================================================================== */

/* coop and preempt: the smallest counter of their five threads */
static u32 tm88_min5(volatile unsigned long a, volatile unsigned long b,
                     volatile unsigned long c, volatile unsigned long d,
                     volatile unsigned long e) {
    unsigned long m = a;
    if (b < m) m = b;
    if (c < m) m = c;
    if (d < m) m = d;
    if (e < m) m = e;
    return (u32)m;
}
static u32 tm88_min_coop(void) {
    return tm88_min5(tm_cooperative_thread_0_counter, tm_cooperative_thread_1_counter,
                     tm_cooperative_thread_2_counter, tm_cooperative_thread_3_counter,
                     tm_cooperative_thread_4_counter);
}
static u32 tm88_min_preempt(void) {
    return tm88_min5(tm_preemptive_thread_0_counter, tm_preemptive_thread_1_counter,
                     tm_preemptive_thread_2_counter, tm_preemptive_thread_3_counter,
                     tm_preemptive_thread_4_counter);
}

/* order_min != NULL: the test's own check (every counter within 1 of the
 * average) states an ORDER - strict round robin for coop, strict priority
 * preemption for preempt - that a fair scheduler does not follow and is not
 * meant to. Its ERROR line is reported as 'order', not failed on; what must
 * hold instead is that none of the five threads starved (min > 0). */
typedef struct {
    const char *name;
    void (*main)(void);
    void (*isr)(void);
    u32  (*order_min)(void);
} tm88_test_t;

static const tm88_test_t s_tests[] = {
    { "basic",       tm88_main_basic,       NULL,                            NULL },
    { "coop",        tm88_main_coop,        NULL,                            tm88_min_coop },
    { "preempt",     tm88_main_preempt,     NULL,                            tm88_min_preempt },
    { "irq",         tm88_main_irq,         tm_interrupt_handler,            NULL },
    { "irq_preempt", tm88_main_irq_preempt, tm_interrupt_preemption_handler, NULL },
    { "message",     tm88_main_message,     NULL,                            NULL },
    { "sync",        tm88_main_sync,        NULL,                            NULL },
    { "memory",      tm88_main_memory,      NULL,                            NULL },
};
#define TM88_NTESTS (sizeof(s_tests) / sizeof(s_tests[0]))

/* The upstream counters are globals that start at 0 once: a second run of
 * the same test would carry the first one's counts into its report. */
static void tm88_reset_counters(void) {
    tm_basic_processing_counter = 0;
    tm_cooperative_thread_0_counter = tm_cooperative_thread_1_counter = 0;
    tm_cooperative_thread_2_counter = tm_cooperative_thread_3_counter = 0;
    tm_cooperative_thread_4_counter = 0;
    tm_preemptive_thread_0_counter = tm_preemptive_thread_1_counter = 0;
    tm_preemptive_thread_2_counter = tm_preemptive_thread_3_counter = 0;
    tm_preemptive_thread_4_counter = 0;
    tm_interrupt_thread_0_counter = tm_interrupt_handler_counter = 0;
    tm_interrupt_preemption_thread_0_counter = 0;
    tm_interrupt_preemption_thread_1_counter = 0;
    tm_interrupt_preemption_handler_counter = 0;
    tm_message_processing_counter = 0;
    tm_synchronization_processing_counter = 0;
    tm_memory_allocation_counter = 0;
}

static bool tm88_run(const tm88_test_t *t, u32 sec) {
    for (u32 i = 0U; i < TM88_THREADS; i++) {
        s_h[i] = CA_TASK_HANDLE_NONE;
        s_go[i] = 0U;
    }
    s_done = false;
    s_failed = false;
    s_errors = 0U;
    s_total = 0U;
    s_have_total = false;
    s_line_len = 0U;
    tm88_reset_counters();
    tm_test_duration = (int)sec;
    tm_test_cycles = 1;
    s_isr = t->isr;

    t->main();                                   /* creates the threads */

    for (u32 ms = 0U; !s_done && ms < (sec + 10U) * 1000U; ms += 50U) {
        ca_os_sleep(50);
    }
    const bool finished = s_done;
    s_isr = NULL;
    /* Read before the threads go: the counters stop with them anyway */
    const u32 min_ctr = (t->order_min != NULL) ? t->order_min() : 0U;
    for (u32 i = 0U; i < TM88_THREADS; i++) {
        if (s_h[i] != CA_TASK_HANDLE_NONE) (void)ca_os_remove_job_handle(s_h[i]);
    }
    ca_os_sleep(50);                             /* removed and freed */

    const u32 per_sec = s_total / sec;
    const u32 errors  = (t->order_min != NULL) ? 0U : s_errors;
    const bool ok = finished && !s_failed && errors == 0U && s_have_total && s_total > 0U &&
                    (t->order_min == NULL || min_ctr > 0U);
    if (t->order_min != NULL) {
        CLI_PRINTF_N("e88 %-11s : total=%u per_sec=%u errors=%u order=%u min=%u%s%s  %s",
                     t->name, s_total, per_sec, errors, s_errors, min_ctr,
                     finished ? "" : " NO_REPORT", s_failed ? " INIT_FAILED" : "",
                     ok ? "PASS" : "FAIL");
    } else {
        CLI_PRINTF_N("e88 %-11s : total=%u per_sec=%u errors=%u%s%s  %s",
                     t->name, s_total, per_sec, errors,
                     finished ? "" : " NO_REPORT", s_failed ? " INIT_FAILED" : "",
                     ok ? "PASS" : "FAIL");
    }
    return ok;
}

/* ============================================================================
 * CLI
 * ========================================================================== */

static void cmd_run(const char *p) {
    int only = -1;
    u32 sec = TM88_SEC_DEFAULT;

    p = ca_skip_sp(p);
    if (ca_strncmp(p, "all", 3) == 0) {
        p = ca_skip_sp(p + 3);
    } else if (*p >= 'a' && *p <= 'z') {
        for (u32 i = 0U; i < TM88_NTESTS; i++) {
            const u32 n = ca_strlen(s_tests[i].name);
            if (ca_strncmp(p, s_tests[i].name, n) == 0 &&
                (p[n] == ' ' || p[n] == '\0' || p[n] == '\r' || p[n] == '\n')) {
                only = (int)i;
                p = ca_skip_sp(p + n);
                break;
            }
        }
        if (only < 0) {
            CLI_PUT_N("e88: unknown test - see 'e88 info'");
            return;
        }
    }
    if (*p >= '0' && *p <= '9') {
        const i32 s = ca_strtol(p, 10);
        if (s >= 1 && s <= 300) sec = (u32)s;
    }

    if (!s_tim_ready) {
        /* Counter never started: only EGR = UG raises the update IRQ */
        s_tim_ready = ca_timer_init_periodic(TM88_TIM, 1000000U, tm88_tim_cb);
        if (!s_tim_ready) {
            CLI_PUT_N("e88: TIM3 init failed");
            CLI_PUT_N("RUN RESULT : FAIL  (0/1)");
            return;
        }
    }

    u32 runs = 0U, pass = 0U;
    for (u32 i = 0U; i < TM88_NTESTS; i++) {
        if (only >= 0 && (int)i != only) continue;
        runs++;
        if (tm88_run(&s_tests[i], sec)) pass++;
    }
    CLI_PRINTF_N("RUN RESULT : %s  (%u/%u)", (pass == runs) ? "OK" : "FAIL", pass, runs);
}

static void cmd_info(void) {
    CLI_PUT_N("--- Example 88: Thread-Metric on caOS ---");
    CLI_PUT_N("  tests : basic coop preempt irq irq_preempt message sync memory");
    CLI_PRINTF_N("  sec   : %u per test by default (upstream publishes 30)", TM88_SEC_DEFAULT);
    CLI_PRINTF_N("  clock : %u Hz", SystemCoreClock);
}

static bool user_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e88       : run [test|all] [sec] | info");
        return false;
    }
    if (ca_strncmp(p, "e88", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (s_busy) {
        CLI_PUT_N("e88: a run is in progress - wait for its RUN RESULT");
        return true;
    }
    if (ca_strncmp(p, "run", 3) == 0) {
        s_busy = true;
        cmd_run(p + 3);
        s_busy = false;
        return true;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        cmd_info();
        return true;
    }
    CLI_PUT_N("e88       : run [test|all] [sec] | info");
    return true;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ============================================================================
 * Entry points
 * ========================================================================== */

static u32 task_main(void *arg) {
    (void)arg;
    ca_os_sleep(500);
    ca_stdout_put_n("example_88: Thread-Metric, idle - 'e88 run' starts the tests");
    ca_stdout_put_n("example_88: ready");
    while (1) {
        ca_os_sleep(1000);
    }
    return 0;
}

void ca_os_configure(void) {
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    ca_cli_init(2048);
    ca_cli_set_sender(STDOUT);
    ca_cli_create_thread();
}

void ca_os_main(void) {
    ca_timer_init();
    ca_os_job("example_88", CA_TH_LEVEL3, task_main, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_cli_register(&s_user_cli_node);
    ca_os_run(true);
}

#else  /* a flag missing, or too few task slots */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m88a, "=== Example 88 ===");
#if (MAX_TASK < 16)
PRIVATE_STRING(m88b, "MAX_TASK too small for Thread-Metric (needs 16 task slots)");
#else
PRIVATE_STRING(m88b, "EN_TIMER / EN_BLOCK_POOL / ENABLE_CLI disabled");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m88a);
    PUT_N(&priv_m88b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
