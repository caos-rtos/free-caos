// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 91 - kernel semantics bench: the cases named by Zephyr's
 * tests/kernel catalogue that caOS did not test (read on 05/10/2026)
 *
 * Every case is deterministic and runs on request:
 *
 *   e91 start [case|all]   one pass, one line per case, ends with RUN RESULT
 *   e91 list               the cases
 *
 * A case is played by up to four ACTOR tasks, each running a small script
 * (lock, timed lock, unlock, wait on a gate, set a mark, record its own
 * priority). The command's task is the director: it creates the actors,
 * waits for their marks, opens the gates and reads their priorities. A
 * script keeps a case to a few lines and makes the order of events explicit.
 *
 * Priority inheritance is checked on the EFFECTIVE priority field, not on who
 * gets the CPU: caOS schedules by weight (CFS), so a low task keeps running
 * next to a middle one and the classic "M starves L" never shows. What PI
 * must guarantee is the priority the owner runs at, and that is readable.
 *
 * Priorities: L = CA_TH_LEVEL1, M = CA_TH_LEVEL3, H = CA_TH_LEVEL5.
 *
 *   pi_basic    L holds M0, H waits on it: L runs at H, back to L at unlock
 *   pi_two_mtx  L holds M0+M1, H waits on M1, L unlocks M0: L stays at H
 *   pi_timeout  H gives up on M0 (timed lock): L drops back without unlocking
 *   pi_chain    L holds M1, M holds M0 and waits M1, H waits M0: L reaches H
 *   pi_setprio  L boosted, set_priority(L, LEVEL2): stays H while boosted,
 *               LEVEL2 after the unlock
 *
 * No wiring.
 */

#include "caos.h"

#if ENABLE_CLI && EN_SW_TIMER && EN_RWLOCK && (MAX_TASK >= 16)

#define E91_ACTORS      8U
#define E91_MUTEXES     6U
#define E91_STEPS       12U
#define E91_WAIT_MS     500U        /* a mark that does not come within this = stuck */
#define E91_SETTLE_MS   30U         /* a blocking call has reached its wait list */

#define PRIO_L          CA_TH_LEVEL1
#define PRIO_M          CA_TH_LEVEL3
#define PRIO_H          CA_TH_LEVEL5

/* ---- actor scripts --------------------------------------------------- */

enum {
    OP_END = 0,
    OP_LOCK,        /* a = mutex                                  */
    OP_TLOCK,       /* a = mutex, b = ms, result -> s_res[a]      */
    OP_UNLOCK,      /* a = mutex                                  */
    OP_GATE,        /* a = gate: wait until the director opens it */
    OP_MARK,        /* a = bit set in s_mark                      */
    OP_PRIO,        /* a = slot of s_rec: own priority right now  */
    OP_RTRY,        /* a = rmutex, trylock result -> s_ok[b]      */
    OP_RUNLOCK,     /* a = rmutex                                 */
    OP_SWAIT,       /* a = sem, no timeout                        */
    OP_STWAIT,      /* a = sem, b = ms, result -> s_ok[slot]      */
    OP_QRECV,       /* b = ms (0 = forever), item -> s_val[slot],
                       result -> s_ok[slot]                       */
    OP_QRECV_T,     /* b = ms: like OP_QRECV, the timeout of this
                       actor comes from s_tmo[slot] instead       */
    OP_T0,          /* s_t0[slot] = now (ms)                      */
    OP_T1,          /* s_t1[slot] = now (ms)                      */
    OP_CWAIT,       /* cond wait on s_cv with mutex 0             */
    OP_CTWAIT,      /* b = ms, result -> s_ok[slot]               */
    OP_OWNER,       /* s_val[slot] = owns mutex 0                 */
    OP_UNLOCK_OWNED /* unlock mutex 0 if owned                    */
};

typedef struct {
    u8  op;
    u8  a;
    u16 b;
} act_op_t;

static const act_op_t *volatile s_script[E91_ACTORS];
static ca_task_handle_t s_h[E91_ACTORS];
static ca_mutex_t          s_mtx[E91_MUTEXES];
static volatile u8      s_gate[8];
static volatile u32     s_mark;
static volatile u32     s_done;
static volatile u16     s_rec[8];
static volatile bool    s_res[E91_MUTEXES];
static volatile bool    s_abort;
static ca_recursive_mutex_t s_rmtx;
static ca_semaphore_t      s_sem;
static CAQUEUE_STATIC_DEFINE(s_q, 8, sizeof(u32));
static volatile bool    s_ok[E91_ACTORS];
static volatile u32     s_val[E91_ACTORS];
static volatile u32     s_tmo[E91_ACTORS];
static volatile u32     s_t0[E91_ACTORS];
static volatile u32     s_t1[E91_ACTORS];
static ca_condvar_t        s_cv;
static volatile bool    s_busy;

static u16 own_priority(void) {
    const ca_thread_context_t *c = ca_scheduler_get_current_context();
    return (c != NULL) ? c->priority : 0U;
}

/* Effective priority of another task, by handle (a thid is only a position). */
static u16 prio_of(ca_task_handle_t h) {
    u16 p = 0U;
    const u32 pm = __get_PRIMASK();
    __disable_irq();
    const u32 idx = ca_os_thid_from_handle(h);
    if (idx != (u32)-1) {
        const ca_thread_context_t *c = ca_scheduler_get_context_from_idx(idx);
        if (c != NULL) p = c->priority;
    }
    __set_PRIMASK(pm);
    return p;
}

static u32 task_actor(void *arg) {
    const u32 slot = (u32)(uintptr_t)arg;
    for (const act_op_t *op = s_script[slot]; op->op != OP_END && !s_abort; op++) {
        switch (op->op) {
        case OP_LOCK:   ca_mutex_lock(&s_mtx[op->a]); break;
        case OP_TLOCK:  s_res[op->a] = ca_mutex_timedlock(&s_mtx[op->a], op->b); break;
        case OP_UNLOCK: ca_mutex_unlock(&s_mtx[op->a]); break;
        case OP_GATE:   while (s_gate[op->a] == 0U && !s_abort) ca_os_sleep(1); break;
        case OP_MARK:   s_mark |= (1UL << op->a); break;
        case OP_PRIO:   s_rec[op->a] = own_priority(); break;
        case OP_RTRY:   s_ok[op->b] = ca_rmutex_trylock(&s_rmtx); break;
        case OP_RUNLOCK: ca_rmutex_unlock(&s_rmtx); break;
        case OP_SWAIT:  ca_sem_wait(&s_sem); s_ok[slot] = true; break;
        case OP_STWAIT: s_ok[slot] = ca_sem_timedwait(&s_sem, op->b); break;
        case OP_QRECV: {
            u32 v = 0U;
            s_ok[slot]  = ca_queue_static_receive(&s_q.q, &v, op->b ? op->b : CA_WAIT_FOREVER);
            s_val[slot] = v;
            break;
        }
        case OP_QRECV_T: {
            u32 v = 0U;
            s_ok[slot]  = ca_queue_static_receive(&s_q.q, &v, s_tmo[slot]);
            s_val[slot] = v;
            break;
        }
        case OP_T0:     s_t0[slot] = ca_os_tick_ms(); break;
        case OP_T1:     s_t1[slot] = ca_os_tick_ms(); break;
        case OP_CWAIT:  ca_cond_wait(&s_cv, &s_mtx[0]); break;
        case OP_CTWAIT: s_ok[slot] = ca_cond_timedwait(&s_cv, &s_mtx[0], op->b); break;
        case OP_OWNER:  s_val[slot] = ca_mutex_is_owner(&s_mtx[0]) ? 1U : 0U; break;
        case OP_UNLOCK_OWNED:
            if (ca_mutex_is_owner(&s_mtx[0])) ca_mutex_unlock(&s_mtx[0]);
            break;
        default: break;
        }
    }
    s_done |= (1UL << slot);
    return 0;
}

/* ---- director helpers ------------------------------------------------- */

static void case_reset(void) {
    for (u32 i = 0U; i < E91_MUTEXES; i++) {
        ca_mutex_init(&s_mtx[i]);
        s_res[i] = false;
    }
    for (u32 i = 0U; i < 8U; i++) { s_gate[i] = 0U; s_rec[i] = 0U; }
    for (u32 i = 0U; i < E91_ACTORS; i++) {
        s_h[i] = CA_TASK_HANDLE_NONE;
        s_ok[i] = false; s_val[i] = 0U; s_tmo[i] = 0U; s_t0[i] = 0U; s_t1[i] = 0U;
    }
    ca_rmutex_init(&s_rmtx);
    ca_cond_init(&s_cv);
    ca_sem_init(&s_sem, "e91", 0U, 8U);
    ca_queue_static_reset(&s_q.q);
    s_mark  = 0U;
    s_done  = 0U;
    s_abort = false;
}

static const char *const s_actor_name[E91_ACTORS] = {
    "e91_a0", "e91_a1", "e91_a2", "e91_a3", "e91_a4", "e91_a5", "e91_a6", "e91_a7"
};

static bool spawn(u32 slot, u16 prio, const act_op_t *script) {
    s_script[slot] = script;
    s_h[slot] = ca_os_job(s_actor_name[slot], prio, task_actor, (void *)(uintptr_t)slot, STACK_SMALL,
                          CA_JOB_STD, CA_FAULT_ASSERT);
    return s_h[slot] != CA_TASK_HANDLE_NONE;
}

static bool wait_mark(u32 bits) {
    for (u32 t = 0U; (s_mark & bits) != bits && t < E91_WAIT_MS; t++) ca_os_sleep(1);
    return (s_mark & bits) == bits;
}

/* Open every gate and give the actors E91_WAIT_MS to finish. One that is
 * still there is stuck in a wait the case did not expect: removed, and the
 * case fails. The mutexes are re-initialised by the next case_reset(). */
static bool finish(u32 used) {
    const u32 all = (1UL << used) - 1UL;
    for (u32 i = 0U; i < 8U; i++) s_gate[i] = 1U;
    for (u32 t = 0U; (s_done & all) != all && t < E91_WAIT_MS; t++) ca_os_sleep(1);
    if ((s_done & all) == all) return true;
    s_abort = true;
    for (u32 i = 0U; i < used; i++) {
        if ((s_done & (1UL << i)) == 0U && s_h[i] != CA_TASK_HANDLE_NONE) {
            (void)ca_os_remove_job_handle(s_h[i]);
        }
    }
    ca_os_sleep(E91_SETTLE_MS);
    return false;
}

#define SAY(name, ok, fmt, ...) \
    CLI_PRINTF_N("[e91] %-11s %s  " fmt, name, (ok) ? "PASS" : "FAIL", __VA_ARGS__)

/* ---- priority inheritance -------------------------------------------- */

static bool case_pi_basic(const char *name) {
    static const act_op_t L[] = { {OP_LOCK,0,0}, {OP_MARK,0,0}, {OP_GATE,0,0},
                                  {OP_UNLOCK,0,0}, {OP_PRIO,0,0}, {OP_END,0,0} };
    static const act_op_t H[] = { {OP_LOCK,0,0}, {OP_UNLOCK,0,0}, {OP_MARK,1,0},
                                  {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_L, L) && wait_mark(1U << 0);
    made = made && spawn(1, PRIO_H, H);
    ca_os_sleep(E91_SETTLE_MS);
    const u16 boosted = prio_of(s_h[0]);
    s_gate[0] = 1U;
    const bool got = wait_mark(1U << 1);
    const bool out = finish(2U);
    const bool ok = made && got && out && boosted == PRIO_H && s_rec[0] == PRIO_L;
    SAY(name, ok, "L while H waits=%u (want %u), L after unlock=%u (want %u)%s",
        boosted, PRIO_H, s_rec[0], PRIO_L, out ? "" : " STUCK");
    return ok;
}

static bool case_pi_two_mutex(const char *name) {
    static const act_op_t L[] = { {OP_LOCK,0,0}, {OP_LOCK,1,0}, {OP_MARK,0,0},
                                  {OP_GATE,0,0}, {OP_UNLOCK,0,0}, {OP_PRIO,0,0},
                                  {OP_MARK,2,0}, {OP_GATE,1,0}, {OP_UNLOCK,1,0},
                                  {OP_PRIO,1,0}, {OP_END,0,0} };
    static const act_op_t H[] = { {OP_LOCK,1,0}, {OP_UNLOCK,1,0}, {OP_MARK,1,0},
                                  {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_L, L) && wait_mark(1U << 0);
    made = made && spawn(1, PRIO_H, H);
    ca_os_sleep(E91_SETTLE_MS);
    const u16 boosted = prio_of(s_h[0]);
    s_gate[0] = 1U;                         /* L releases M0, still holds M1 */
    const bool mid = wait_mark(1U << 2);
    s_gate[1] = 1U;
    const bool got = wait_mark(1U << 1);
    const bool out = finish(2U);
    const bool ok = made && mid && got && out && boosted == PRIO_H &&
                    s_rec[0] == PRIO_H && s_rec[1] == PRIO_L;
    SAY(name, ok, "L boosted=%u, after unlock M0 (H still on M1)=%u (want %u), "
        "after unlock M1=%u (want %u)%s", boosted, s_rec[0], PRIO_H, s_rec[1], PRIO_L,
        out ? "" : " STUCK");
    return ok;
}

static bool case_pi_timeout(const char *name) {
    static const act_op_t L[] = { {OP_LOCK,0,0}, {OP_MARK,0,0}, {OP_GATE,0,0},
                                  {OP_UNLOCK,0,0}, {OP_END,0,0} };
    static const act_op_t H[] = { {OP_TLOCK,0,50}, {OP_MARK,1,0}, {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_L, L) && wait_mark(1U << 0);
    made = made && spawn(1, PRIO_H, H);
    ca_os_sleep(E91_SETTLE_MS);
    const u16 boosted = prio_of(s_h[0]);
    const bool gave_up = wait_mark(1U << 1);
    ca_os_sleep(5);
    const u16 after = prio_of(s_h[0]);      /* L still holds M0 */
    const bool out = finish(2U);
    const bool ok = made && gave_up && out && !s_res[0] &&
                    boosted == PRIO_H && after == PRIO_L;
    SAY(name, ok, "L boosted=%u, after H timed out (L still owner)=%u (want %u)%s%s",
        boosted, after, PRIO_L, s_res[0] ? " H_GOT_IT" : "", out ? "" : " STUCK");
    return ok;
}

static bool case_pi_chain(const char *name) {
    static const act_op_t L[] = { {OP_LOCK,1,0}, {OP_MARK,0,0}, {OP_GATE,0,0},
                                  {OP_UNLOCK,1,0}, {OP_END,0,0} };
    static const act_op_t M[] = { {OP_LOCK,0,0}, {OP_MARK,1,0}, {OP_LOCK,1,0},
                                  {OP_UNLOCK,1,0}, {OP_UNLOCK,0,0}, {OP_END,0,0} };
    static const act_op_t H[] = { {OP_LOCK,0,0}, {OP_UNLOCK,0,0}, {OP_MARK,2,0},
                                  {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_L, L) && wait_mark(1U << 0);
    made = made && spawn(1, PRIO_M, M) && wait_mark(1U << 1);
    ca_os_sleep(E91_SETTLE_MS);             /* M waits on M1: L at M */
    made = made && spawn(2, PRIO_H, H);
    ca_os_sleep(E91_SETTLE_MS);             /* H waits on M0: M at H, and L? */
    const u16 pm = prio_of(s_h[1]);
    const u16 pl = prio_of(s_h[0]);
    s_gate[0] = 1U;
    const bool got = wait_mark(1U << 2);
    const bool out = finish(3U);
    const bool ok = made && got && out && pm == PRIO_H && pl == PRIO_H;
    SAY(name, ok, "M (owner of M0)=%u (want %u), L (owner of M1, M waits)=%u (want %u)%s",
        pm, PRIO_H, pl, PRIO_H, out ? "" : " STUCK");
    return ok;
}

static bool case_pi_setprio(const char *name) {
    static const act_op_t L[] = { {OP_LOCK,0,0}, {OP_MARK,0,0}, {OP_GATE,0,0},
                                  {OP_UNLOCK,0,0}, {OP_PRIO,0,0}, {OP_END,0,0} };
    static const act_op_t H[] = { {OP_LOCK,0,0}, {OP_UNLOCK,0,0}, {OP_MARK,1,0},
                                  {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_L, L) && wait_mark(1U << 0);
    made = made && spawn(1, PRIO_H, H);
    ca_os_sleep(E91_SETTLE_MS);
    const u16 boosted = prio_of(s_h[0]);
    const bool set = ca_os_set_priority_handle(s_h[0], CA_TH_LEVEL2);
    const u16 during = prio_of(s_h[0]);
    s_gate[0] = 1U;
    const bool got = wait_mark(1U << 1);
    const bool out = finish(2U);
    const bool ok = made && set && got && out && boosted == PRIO_H &&
                    during == PRIO_H && s_rec[0] == CA_TH_LEVEL2;
    SAY(name, ok, "set to %u while boosted: during=%u (want %u), after unlock=%u (want %u)%s",
        CA_TH_LEVEL2, during, PRIO_H, s_rec[0], CA_TH_LEVEL2, out ? "" : " STUCK");
    return ok;
}

/* H, the waiter that boosted L, is removed while it waits: L drops back. */
static bool case_pi_kill(const char *name) {
    static const act_op_t L[] = { {OP_LOCK,0,0}, {OP_MARK,0,0}, {OP_GATE,0,0},
                                  {OP_UNLOCK,0,0}, {OP_END,0,0} };
    static const act_op_t H[] = { {OP_LOCK,0,0}, {OP_UNLOCK,0,0}, {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_L, L) && wait_mark(1U << 0);
    made = made && spawn(1, PRIO_H, H);
    ca_os_sleep(E91_SETTLE_MS);
    const u16 boosted = prio_of(s_h[0]);
    const bool removed = ca_os_remove_job_handle(s_h[1]);
    ca_os_sleep(E91_SETTLE_MS);
    const u16 after = prio_of(s_h[0]);
    s_done |= (1UL << 1);                   /* H will never set it */
    const bool out = finish(2U);
    const bool ok = made && removed && out && boosted == PRIO_H && after == PRIO_L;
    SAY(name, ok, "L boosted=%u, after H removed while waiting=%u (want %u)%s",
        boosted, after, PRIO_L, out ? "" : " STUCK");
    return ok;
}

/* Six owners in a chain: A0 holds m0, Ai holds mi and waits m(i-1), H waits
 * m5. The boost walks CA_PI_MAX_CHAIN hops past the direct owner and stops:
 * the owners beyond the cap keep their priority, and nothing hangs. */
static bool case_pi_hopcap(const char *name) {
    static const act_op_t A0[] = { {OP_LOCK,0,0}, {OP_MARK,0,0}, {OP_GATE,0,0},
                                   {OP_UNLOCK,0,0}, {OP_END,0,0} };
    static const act_op_t A[5][6] = {
        { {OP_LOCK,1,0}, {OP_MARK,1,0}, {OP_LOCK,0,0}, {OP_UNLOCK,0,0}, {OP_UNLOCK,1,0}, {OP_END,0,0} },
        { {OP_LOCK,2,0}, {OP_MARK,2,0}, {OP_LOCK,1,0}, {OP_UNLOCK,1,0}, {OP_UNLOCK,2,0}, {OP_END,0,0} },
        { {OP_LOCK,3,0}, {OP_MARK,3,0}, {OP_LOCK,2,0}, {OP_UNLOCK,2,0}, {OP_UNLOCK,3,0}, {OP_END,0,0} },
        { {OP_LOCK,4,0}, {OP_MARK,4,0}, {OP_LOCK,3,0}, {OP_UNLOCK,3,0}, {OP_UNLOCK,4,0}, {OP_END,0,0} },
        { {OP_LOCK,5,0}, {OP_MARK,5,0}, {OP_LOCK,4,0}, {OP_UNLOCK,4,0}, {OP_UNLOCK,5,0}, {OP_END,0,0} },
    };
    static const act_op_t H[] = { {OP_LOCK,5,0}, {OP_UNLOCK,5,0}, {OP_MARK,6,0}, {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_L, A0) && wait_mark(1U << 0);
    for (u32 i = 0U; i < 5U && made; i++) {
        made = spawn(i + 1U, PRIO_L, A[i]) && wait_mark(1UL << (i + 1U));
        ca_os_sleep(5);                     /* A(i+1) is waiting on m(i) */
    }
    made = made && spawn(6, PRIO_H, H);
    ca_os_sleep(E91_SETTLE_MS);
    u16 p[6];
    u32 reached = 0U;
    for (u32 i = 0U; i < 6U; i++) {
        p[i] = prio_of(s_h[i]);
        if (p[i] == PRIO_H) reached++;
    }
    s_gate[0] = 1U;
    const bool got = wait_mark(1U << 6);
    const bool out = finish(7U);
    /* direct owner A5 + CA_PI_MAX_CHAIN hops = A5..A1 boosted, A0 not */
    const u32 want = CA_PI_MAX_CHAIN + 1U;
    const bool ok = made && got && out && reached == want && p[5] == PRIO_H &&
                    p[0] == PRIO_L;
    SAY(name, ok, "chain of 6 owners, cap %u hops: boosted %u (want %u) - A0..A5 = %u %u %u %u %u %u%s",
        CA_PI_MAX_CHAIN, reached, want, p[0], p[1], p[2], p[3], p[4], p[5], out ? "" : " STUCK");
    return ok;
}

/* ---- waits and queues ------------------------------------------------- */

/* The director owns the recursive mutex three deep; another task's trylock
 * must fail until the count is back to zero, not at the first unlock. */
static bool case_rmutex_count(const char *name) {
    static const act_op_t A[] = { {OP_RTRY,0,0}, {OP_MARK,0,0}, {OP_GATE,0,0},
                                  {OP_RTRY,0,1}, {OP_MARK,1,0}, {OP_GATE,1,0},
                                  {OP_RTRY,0,2}, {OP_MARK,2,0}, {OP_END,0,0} };
    case_reset();
    ca_rmutex_lock(&s_rmtx);
    ca_rmutex_lock(&s_rmtx);
    ca_rmutex_lock(&s_rmtx);
    bool made = spawn(0, PRIO_M, A);
    const bool m0 = wait_mark(1U << 0);     /* count 3 */
    ca_rmutex_unlock(&s_rmtx);
    ca_rmutex_unlock(&s_rmtx);
    s_gate[0] = 1U;
    const bool m1 = wait_mark(1U << 1);     /* count 1 */
    ca_rmutex_unlock(&s_rmtx);
    s_gate[1] = 1U;
    const bool m2 = wait_mark(1U << 2);     /* count 0 */
    const bool got3 = s_ok[2];               /* the actor ends holding it:
                                                case_reset() re-initialises */
    const bool out = finish(1U);
    const bool ok = made && m0 && m1 && m2 && out && !s_ok[0] && !s_ok[1] && got3;
    SAY(name, ok, "trylock by another task at count 3=%u 1=%u 0=%u (want 0 0 1)%s",
        s_ok[0], s_ok[1], got3, out ? "" : " STUCK");
    return ok;
}

/* ca_os_wake_up_handle on tasks waiting on a semaphore, one untimed and one
 * timed. Whatever the wake does, it must not hand out a count nobody posted,
 * nor leave a task linked in the list after it returned: two posts later the
 * books must balance and nobody may be left waiting. */
static bool case_wake_pending(const char *name) {
    static const act_op_t A[] = { {OP_SWAIT,0,0}, {OP_MARK,0,0}, {OP_END,0,0} };
    static const act_op_t B[] = { {OP_STWAIT,0,1000}, {OP_MARK,1,0}, {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_M, A) && spawn(1, PRIO_M, B);
    ca_os_sleep(E91_SETTLE_MS);
    ca_os_wake_up_handle(s_h[0]);
    ca_os_wake_up_handle(s_h[1]);
    ca_os_sleep(E91_SETTLE_MS);
    const bool a_early = (s_mark & (1U << 0)) != 0U;
    const bool b_early = (s_mark & (1U << 1)) != 0U;
    const bool b_early_ok = b_early && s_ok[1];
    ca_sem_post(&s_sem);
    ca_sem_post(&s_sem);
    const bool both = wait_mark((1U << 0) | (1U << 1));
    const u32 takers = (s_ok[0] ? 1U : 0U) + (s_ok[1] ? 1U : 0U);
    const u32 left = ca_sem_getvalue(&s_sem);
    const bool out = finish(2U);
    const bool ok = made && both && out && !a_early && !b_early_ok &&
                    takers + left == 2U && s_ok[0];
    SAY(name, ok, "after wake: untimed %s, timed %s; after 2 posts: taken=%u left=%u%s",
        a_early ? "RETURNED" : "still waiting",
        b_early ? (s_ok[1] ? "RETURNED WITH A COUNT" : "returned empty") : "still waiting",
        takers, left, out ? "" : " STUCK");
    return ok;
}

/* reset of a queue with two receivers waiting: they must not come out with
 * an item nobody sent; the next send reaches one of them, the second the
 * other. */
static bool case_reset_waiters(const char *name) {
    static const act_op_t A[] = { {OP_QRECV,0,0}, {OP_MARK,0,0}, {OP_END,0,0} };
    static const act_op_t B[] = { {OP_QRECV,0,0}, {OP_MARK,1,0}, {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_M, A) && spawn(1, PRIO_M, B);
    ca_os_sleep(E91_SETTLE_MS);
    ca_queue_static_reset(&s_q.q);
    ca_os_sleep(E91_SETTLE_MS);
    const u32 ghost = s_mark & 3U;
    u32 v = 0x55U;
    (void)ca_queue_static_send(&s_q.q, &v, 10U);
    ca_os_sleep(E91_SETTLE_MS);
    const u32 one = s_mark & 3U;
    v = 0x66U;
    (void)ca_queue_static_send(&s_q.q, &v, 10U);
    const bool both = wait_mark(3U);
    const bool out = finish(2U);
    const bool vals = (s_val[0] == 0x55U && s_val[1] == 0x66U) ||
                      (s_val[0] == 0x66U && s_val[1] == 0x55U);
    const bool ok = made && out && both && ghost == 0U && (one == 1U || one == 2U) &&
                    vals && s_ok[0] && s_ok[1];
    SAY(name, ok, "after reset out=%u (want 0), after 1st send out=%u (want 1), "
        "values %X %X%s", (ghost & 1U) + (ghost >> 1), (one & 1U) + (one >> 1),
        s_val[0], s_val[1], out ? "" : " STUCK");
    return ok;
}

static bool case_send_front(const char *name) {
    case_reset();
    u32 v;
    v = 1U; const bool s1 = ca_queue_static_send(&s_q.q, &v, 10U);
    v = 2U; const bool s2 = ca_queue_static_send(&s_q.q, &v, 10U);
    v = 3U; const bool s3 = ca_queue_static_send_front(&s_q.q, &v, 10U);
    u32 r[3] = { 0U, 0U, 0U };
    bool rx = true;
    for (u32 i = 0U; i < 3U; i++) rx = ca_queue_static_receive(&s_q.q, &r[i], 10U) && rx;
    const bool ok = s1 && s2 && s3 && rx && r[0] == 3U && r[1] == 1U && r[2] == 2U;
    SAY(name, ok, "send 1, send 2, send_front 3 -> %u %u %u (want 3 1 2)", r[0], r[1], r[2]);
    return ok;
}

/* Eight timed receivers on an empty queue, timeouts in scrambled order: each
 * comes back no earlier than its timeout and at most E91_TMO_SLACK_MS later,
 * so they come out in deadline order. */
#define E91_TMO_SLACK_MS  3U
static bool case_scrambled_tmo(const char *name) {
    static const act_op_t A[] = { {OP_T0,0,0}, {OP_QRECV_T,0,0}, {OP_T1,0,0},
                                  {OP_MARK,0,0}, {OP_END,0,0} };
    static const u32 tmo[E91_ACTORS] = { 70U, 10U, 50U, 30U, 80U, 20U, 60U, 40U };
    case_reset();
    bool made = true;
    for (u32 i = 0U; i < E91_ACTORS && made; i++) {
        s_tmo[i] = tmo[i];
        made = spawn(i, PRIO_M, A);
    }
    ca_os_sleep(150);
    const bool out = finish(E91_ACTORS);
    u32 early = 0U, late = 0U, worst = 0U, got = 0U;
    for (u32 i = 0U; i < E91_ACTORS; i++) {
        const u32 dt = s_t1[i] - s_t0[i];
        if (s_ok[i]) got++;
        if (dt < tmo[i]) {
            early++;
        } else {
            const u32 over = dt - tmo[i];
            if (over > worst) worst = over;
            if (over > E91_TMO_SLACK_MS) late++;
        }
    }
    /* deadline order: a later deadline never returns before an earlier one */
    u32 disorder = 0U;
    for (u32 i = 0U; i < E91_ACTORS; i++) {
        for (u32 j = 0U; j < E91_ACTORS; j++) {
            if (s_t0[i] + tmo[i] < s_t0[j] + tmo[j] && (i32)(s_t1[i] - s_t1[j]) > 0) disorder++;
        }
    }
    const bool ok = made && out && got == 0U && early == 0U && late == 0U && disorder == 0U;
    SAY(name, ok, "8 timeouts 10..80 ms: early=%u late=%u (worst +%u ms, slack %u) "
        "out_of_order=%u%s", early, late, worst, E91_TMO_SLACK_MS, disorder,
        out ? "" : " STUCK");
    return ok;
}

/* ---- condition variable, rwlock, cooperative spinlock -------------------- */

/* Three waiters on the condvar; marks 4..6 say "about to wait", 0..2 "out". */
static const act_op_t s_cv_waiter[3][6] = {
    { {OP_LOCK,0,0}, {OP_MARK,4,0}, {OP_CWAIT,0,0}, {OP_MARK,0,0}, {OP_UNLOCK,0,0}, {OP_END,0,0} },
    { {OP_LOCK,0,0}, {OP_MARK,5,0}, {OP_CWAIT,0,0}, {OP_MARK,1,0}, {OP_UNLOCK,0,0}, {OP_END,0,0} },
    { {OP_LOCK,0,0}, {OP_MARK,6,0}, {OP_CWAIT,0,0}, {OP_MARK,2,0}, {OP_UNLOCK,0,0}, {OP_END,0,0} },
};

static u32 marks_out(void) {
    return ((s_mark >> 0) & 1U) + ((s_mark >> 1) & 1U) + ((s_mark >> 2) & 1U);
}

static bool case_cond_signal_one(const char *name) {
    case_reset();
    bool made = true;
    for (u32 i = 0U; i < 3U && made; i++) made = spawn(i, PRIO_M, s_cv_waiter[i]);
    made = made && wait_mark(0x70U);
    ca_os_sleep(E91_SETTLE_MS);             /* all three inside ca_cond_wait */
    ca_mutex_lock(&s_mtx[0]);
    ca_cond_signal(&s_cv);
    ca_mutex_unlock(&s_mtx[0]);
    ca_os_sleep(E91_SETTLE_MS);
    const u32 one = marks_out();
    ca_mutex_lock(&s_mtx[0]);
    ca_cond_broadcast(&s_cv);               /* release the other two */
    ca_mutex_unlock(&s_mtx[0]);
    const bool all = wait_mark(7U);
    const bool out = finish(3U);
    const bool ok = made && all && out && one == 1U;
    SAY(name, ok, "3 waiters, 1 signal -> %u out (want 1)%s", one, out ? "" : " STUCK");
    return ok;
}

static bool case_cond_broadcast(const char *name) {
    case_reset();
    bool made = true;
    for (u32 i = 0U; i < 3U && made; i++) made = spawn(i, PRIO_M, s_cv_waiter[i]);
    made = made && wait_mark(0x70U);
    ca_os_sleep(E91_SETTLE_MS);
    ca_mutex_lock(&s_mtx[0]);
    ca_cond_broadcast(&s_cv);
    ca_mutex_unlock(&s_mtx[0]);
    ca_os_sleep(E91_SETTLE_MS);
    const u32 n = marks_out();
    const bool out = finish(3U);
    const bool ok = made && out && n == 3U;
    SAY(name, ok, "3 waiters, 1 broadcast -> %u out (want 3)%s", n, out ? "" : " STUCK");
    return ok;
}

/* A timed wait that times out returns owning the mutex again. */
static bool case_cond_tmo_relock(const char *name) {
    static const act_op_t A[] = { {OP_LOCK,0,0}, {OP_T0,0,0}, {OP_CTWAIT,0,30},
                                  {OP_T1,0,0}, {OP_OWNER,0,0}, {OP_UNLOCK_OWNED,0,0},
                                  {OP_MARK,0,0}, {OP_END,0,0} };
    case_reset();
    bool made = spawn(0, PRIO_M, A);
    const bool got = wait_mark(1U << 0);
    const bool out = finish(1U);
    const u32 dt = s_t1[0] - s_t0[0];
    const bool ok = made && got && out && !s_ok[0] && s_val[0] == 1U && dt >= 30U;
    SAY(name, ok, "timedwait 30 ms -> %s after %u ms, owns the mutex=%u (want 1)%s",
        s_ok[0] ? "SIGNALLED" : "timeout", dt, s_val[0], out ? "" : " STUCK");
    return ok;
}

/* rwlock: one writer fills a block with one value, yielding between words to
 * open the window; three readers check every block they see is uniform. Under
 * steady reading the writer must still get through. */
#define E91_RW_WORDS     16U
#define E91_RW_MS        500U
#define E91_RW_MIN_W     20U
static ca_rwlock_t         s_rw;
static volatile u32     s_rw_data[E91_RW_WORDS];
static volatile bool    s_rw_run;
static volatile u32     s_rw_writes, s_rw_reads, s_rw_torn;

static u32 task_rw_writer(void *arg) {
    const u32 slot = (u32)(uintptr_t)arg;
    u32 v = 1U;
    while (s_rw_run) {
        ca_rwlock_write_lock(&s_rw);
        for (u32 i = 0U; i < E91_RW_WORDS; i++) {
            s_rw_data[i] = v;
            if ((i & 3U) == 3U) ca_os_yield();
        }
        ca_rwlock_write_unlock(&s_rw);
        v++;
        s_rw_writes++;
        ca_os_yield();
    }
    s_done |= (1UL << slot);
    return 0;
}

static u32 task_rw_reader(void *arg) {
    const u32 slot = (u32)(uintptr_t)arg;
    while (s_rw_run) {
        ca_rwlock_read_lock(&s_rw);
        const u32 first = s_rw_data[0];
        for (u32 i = 1U; i < E91_RW_WORDS; i++) {
            if (s_rw_data[i] != first) { s_rw_torn++; break; }
            if ((i & 3U) == 3U) ca_os_yield();
        }
        ca_rwlock_read_unlock(&s_rw);
        s_rw_reads++;
    }
    s_done |= (1UL << slot);
    return 0;
}

static bool spawn_fn(u32 slot, ca_task_fn_t fn) {
    s_h[slot] = ca_os_job(s_actor_name[slot], PRIO_M, fn, (void *)(uintptr_t)slot, STACK_SMALL,
                          CA_JOB_STD, CA_FAULT_ASSERT);
    return s_h[slot] != CA_TASK_HANDLE_NONE;
}

static bool case_rwlock_excl(const char *name) {
    case_reset();
    ca_rwlock_init(&s_rw);
    for (u32 i = 0U; i < E91_RW_WORDS; i++) s_rw_data[i] = 0U;
    s_rw_writes = s_rw_reads = s_rw_torn = 0U;
    s_rw_run = true;
    bool made = spawn_fn(0, task_rw_reader) && spawn_fn(1, task_rw_reader) &&
                spawn_fn(2, task_rw_reader) && spawn_fn(3, task_rw_writer);
    ca_os_sleep(E91_RW_MS);
    s_rw_run = false;
    const bool out = finish(4U);
    const bool ok = made && out && s_rw_torn == 0U && s_rw_writes >= E91_RW_MIN_W &&
                    s_rw_reads > 0U;
    SAY(name, ok, "%u ms: writes=%u (min %u) reads=%u torn=%u (want 0)%s", E91_RW_MS,
        s_rw_writes, E91_RW_MIN_W, s_rw_reads, s_rw_torn, out ? "" : " STUCK");
    return ok;
}

/* Cooperative spinlock: three tasks do read-yield-write on a shared counter
 * inside the lock. Exclusion holds if no increment is lost; the holder must
 * progress while the others yield on the busy lock. */
#define E91_SPIN_N       300U
static ca_spinlock_t       s_coop;
static volatile u32     s_coop_count;

static u32 task_coop(void *arg) {
    const u32 slot = (u32)(uintptr_t)arg;
    for (u32 i = 0U; i < E91_SPIN_N; i++) {
        ca_spinlock_lock_cooperative(&s_coop);
        const u32 c = s_coop_count;
        ca_os_yield();
        s_coop_count = c + 1U;
        ca_spinlock_unlock_cooperative(&s_coop);
    }
    s_done |= (1UL << slot);
    return 0;
}

static bool case_coop_spin(const char *name) {
    case_reset();
    ca_spinlock_cooperative_init(&s_coop);
    s_coop_count = 0U;
    bool made = spawn_fn(0, task_coop) && spawn_fn(1, task_coop) && spawn_fn(2, task_coop);
    for (u32 t = 0U; (s_done & 7U) != 7U && t < 5000U; t += 5U) ca_os_sleep(5);
    const bool out = finish(3U);
    const bool ok = made && out && s_coop_count == 3U * E91_SPIN_N;
    SAY(name, ok, "3 x %u locked increments with a yield inside -> %u (want %u)%s",
        E91_SPIN_N, s_coop_count, 3U * E91_SPIN_N, out ? "" : " STUCK");
    return ok;
}

/* ---- software timers ---------------------------------------------------- */

#define E91_TMR_N        8U
#define E91_TMR_SLACK    2U          /* ms */
static ca_sw_timer_handle_t    s_tmr[E91_TMR_N];
static volatile u32     s_tmr_hits[E91_TMR_N];
static volatile u32     s_tmr_first[E91_TMR_N];
static volatile u32     s_tmr_last[E91_TMR_N];
static volatile u32     s_tmr_prev_gap[E91_TMR_N];
static volatile u32     s_tmr_gap_lo, s_tmr_gap_hi;  /* gaps after set_period */
static volatile bool    s_tmr_watch_gap;
static volatile u32     s_tmr_stop_at;               /* stop_in_cb: stop on this hit */
static volatile u32     s_tmr_rearm_until;           /* rearm_cb: restart until this hit */

static void tmr_cb(void *arg) {
    const u32 i = (u32)(uintptr_t)arg;
    const u32 now = ca_os_tick_ms();
    if (s_tmr_hits[i] == 0U) s_tmr_first[i] = now;
    else {
        const u32 gap = now - s_tmr_last[i];
        s_tmr_prev_gap[i] = gap;
        if (s_tmr_watch_gap) {
            if (gap < s_tmr_gap_lo) s_tmr_gap_lo = gap;
            if (gap > s_tmr_gap_hi) s_tmr_gap_hi = gap;
        }
    }
    s_tmr_last[i] = now;
    s_tmr_hits[i]++;
    if (s_tmr_stop_at != 0U && s_tmr_hits[i] == s_tmr_stop_at) (void)ca_sw_timer_stop(s_tmr[i]);
    if (s_tmr_hits[i] < s_tmr_rearm_until) (void)ca_sw_timer_start(s_tmr[i]);
}

static void tmr_reset_all(void) {
    for (u32 i = 0U; i < E91_TMR_N; i++) {
        s_tmr[i] = CA_TIMER_INVALID;
        s_tmr_hits[i] = s_tmr_first[i] = s_tmr_last[i] = s_tmr_prev_gap[i] = 0U;
    }
    s_tmr_watch_gap = false;
    s_tmr_gap_lo = 0xFFFFFFFFU;
    s_tmr_gap_hi = 0U;
    s_tmr_stop_at = 0U;
    s_tmr_rearm_until = 0U;
}

static bool tmr_make(u32 n, u32 ms, ca_sw_timer_mode_t mode) {
    bool ok = true;
    for (u32 i = 0U; i < n; i++) {
        s_tmr[i] = ca_sw_timer_create(ms, mode, tmr_cb, (void *)(uintptr_t)i);
        ok = ok && s_tmr[i] != CA_TIMER_INVALID;
    }
    return ok;
}

static void tmr_free(u32 n) {
    for (u32 i = 0U; i < n; i++) {
        if (s_tmr[i] != CA_TIMER_INVALID) {
            (void)ca_sw_timer_stop(s_tmr[i]);
            (void)ca_sw_timer_delete(s_tmr[i]);
            s_tmr[i] = CA_TIMER_INVALID;
        }
    }
}

/* periodic 10 ms for 2 s: the count and the span must not drift */
static bool case_tmr_drift(const char *name) {
    tmr_reset_all();
    const bool made = tmr_make(1U, 10U, CA_TIMER_PERIODIC) && ca_sw_timer_start(s_tmr[0]);
    ca_os_sleep(2005U);
    (void)ca_sw_timer_stop(s_tmr[0]);
    const u32 n = s_tmr_hits[0];
    const u32 span = s_tmr_last[0] - s_tmr_first[0];
    const u32 want = (n > 0U) ? (n - 1U) * 10U : 0U;
    const u32 drift = (span > want) ? span - want : want - span;
    tmr_free(1U);
    const bool ok = made && n >= 199U && n <= 201U && drift <= E91_TMR_SLACK;
    SAY(name, ok, "10 ms for 2 s: hits=%u (want 200 +-1), span drift=%u ms (max %u)",
        n, drift, E91_TMR_SLACK);
    return ok;
}

/* one-shot 50 ms, reset after 30 ms: fires 50 ms after the reset */
static bool case_tmr_restart(const char *name) {
    tmr_reset_all();
    const bool made = tmr_make(1U, 50U, CA_TIMER_ONE_SHOT);
    const u32 t0 = ca_os_tick_ms();
    (void)ca_sw_timer_start(s_tmr[0]);
    ca_os_sleep(30U);
    const u32 tr = ca_os_tick_ms();
    (void)ca_sw_timer_reset(s_tmr[0]);
    ca_os_sleep(100U);
    const u32 at = s_tmr_first[0] - tr;
    const u32 hits = s_tmr_hits[0];
    tmr_free(1U);
    const bool ok = made && hits == 1U && at + E91_TMR_SLACK >= 50U && at <= 50U + E91_TMR_SLACK;
    SAY(name, ok, "reset at +%u ms -> fired %u ms after the reset (want 50 +-%u), hits=%u",
        tr - t0, at, E91_TMR_SLACK, hits);
    return ok;
}

/* periodic 10 ms that stops itself on its 3rd expiry */
static bool case_tmr_stop_in_cb(const char *name) {
    tmr_reset_all();
    s_tmr_stop_at = 3U;
    const bool made = tmr_make(1U, 10U, CA_TIMER_PERIODIC) && ca_sw_timer_start(s_tmr[0]);
    ca_os_sleep(120U);
    const u32 hits = s_tmr_hits[0];
    const bool stopped = ca_sw_timer_get_state(s_tmr[0]) != CA_TIMER_RUNNING;
    tmr_free(1U);
    const bool ok = made && hits == 3U && stopped;
    SAY(name, ok, "stop from its own callback at hit 3 -> hits=%u (want 3) state=%s",
        hits, stopped ? "stopped" : "RUNNING");
    return ok;
}

/* one-shot 20 ms that re-arms itself from its callback until the 3rd expiry */
static bool case_tmr_rearm_in_cb(const char *name) {
    tmr_reset_all();
    s_tmr_rearm_until = 3U;
    const bool made = tmr_make(1U, 20U, CA_TIMER_ONE_SHOT) && ca_sw_timer_start(s_tmr[0]);
    ca_os_sleep(150U);
    const u32 hits = s_tmr_hits[0];
    const u32 gap = s_tmr_prev_gap[0];
    tmr_free(1U);
    const bool ok = made && hits == 3U && gap + E91_TMR_SLACK >= 20U && gap <= 20U + E91_TMR_SLACK;
    SAY(name, ok, "one-shot re-armed from its own callback until hit 3 -> hits=%u (want 3), "
        "last gap=%u ms", hits, gap);
    return ok;
}

/* eight one-shots armed together: all fire, within the slack of each other */
static bool case_tmr_same_tick(const char *name) {
    tmr_reset_all();
    bool made = tmr_make(E91_TMR_N, 50U, CA_TIMER_ONE_SHOT);
    for (u32 i = 0U; i < E91_TMR_N && made; i++) made = ca_sw_timer_start(s_tmr[i]);
    ca_os_sleep(100U);
    u32 fired = 0U, lo = 0xFFFFFFFFU, hi = 0U;
    for (u32 i = 0U; i < E91_TMR_N; i++) {
        if (s_tmr_hits[i] == 1U) fired++;
        if (s_tmr_first[i] < lo) lo = s_tmr_first[i];
        if (s_tmr_first[i] > hi) hi = s_tmr_first[i];
    }
    tmr_free(E91_TMR_N);
    const bool ok = made && fired == E91_TMR_N && hi - lo <= E91_TMR_SLACK;
    SAY(name, ok, "8 one-shots of 50 ms armed together: fired=%u (want 8), spread=%u ms (max %u)",
        fired, hi - lo, E91_TMR_SLACK);
    return ok;
}

/* periodic 20 ms, set_period(40) while running: from the next cycle on the
 * gap is 40 (the cycle in progress may still be 20) */
static bool case_tmr_period(const char *name) {
    tmr_reset_all();
    const bool made = tmr_make(1U, 20U, CA_TIMER_PERIODIC) && ca_sw_timer_start(s_tmr[0]);
    ca_os_sleep(70U);
    const bool set = ca_sw_timer_set_period(s_tmr[0], 40U);
    const u32 h0 = s_tmr_hits[0];
    for (u32 t = 0U; s_tmr_hits[0] < h0 + 2U && t < 100U; t++) ca_os_sleep(1);
    s_tmr_watch_gap = true;                 /* gaps from here on: all 40 */
    ca_os_sleep(200U);
    (void)ca_sw_timer_stop(s_tmr[0]);
    const u32 lo = s_tmr_gap_lo, hi = s_tmr_gap_hi;
    tmr_free(1U);
    const bool ok = made && set && lo != 0xFFFFFFFFU &&
                    lo + E91_TMR_SLACK >= 40U && hi <= 40U + E91_TMR_SLACK;
    SAY(name, ok, "20 -> 40 ms while running: gaps after it %u..%u ms (want 40 +-%u)",
        lo == 0xFFFFFFFFU ? 0U : lo, hi, E91_TMR_SLACK);
    return ok;
}

/* ---- FPU context shared by preempted tasks -------------------------------- */

#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
#define E91_FP_TASKS     3U
#define E91_FP_MS        2000U
static volatile bool    s_fp_run;
static volatile u32     s_fp_rounds[E91_FP_TASKS];
static volatile u32     s_fp_bad[E91_FP_TASKS];
static volatile u32     s_spin_rounds;

/* Leibniz-style series on eight accumulators: enough live S registers that a
 * context switch which loses or mixes them changes the sum. Deterministic, so
 * every round must give the same bits. */
static u32 fp_series(u32 seed) {
    float a[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    for (u32 n = 0U; n < 300U; n++) {
        const float sgn = (n & 1U) ? -1.0f : 1.0f;
        for (u32 k = 0U; k < 8U; k++) {
            a[k] += sgn / (float)(2U * n + 1U + k + seed);
        }
    }
    const float sum = ((a[0] + a[1]) + (a[2] + a[3])) + ((a[4] + a[5]) + (a[6] + a[7]));
    union { float f; u32 u; } b = { .f = sum };
    return b.u;
}

static u32 task_fp(void *arg) {
    const u32 slot = (u32)(uintptr_t)arg;
    const u32 ref = fp_series(slot);
    while (s_fp_run) {
        if (fp_series(slot) != ref) s_fp_bad[slot]++;
        s_fp_rounds[slot]++;
    }
    s_done |= (1UL << slot);
    return 0;
}

static u32 task_spin(void *arg) {      /* no FP context: mixes the switches */
    const u32 slot = (u32)(uintptr_t)arg;
    while (s_fp_run) {
        s_spin_rounds++;
        if ((s_spin_rounds & 63U) == 0U) ca_os_yield();
    }
    s_done |= (1UL << slot);
    return 0;
}

static bool case_fpu_share(const char *name) {
    case_reset();
    s_fp_run = true;
    s_spin_rounds = 0U;
    bool made = true;
    for (u32 i = 0U; i < E91_FP_TASKS && made; i++) {
        s_fp_rounds[i] = s_fp_bad[i] = 0U;
        s_h[i] = ca_os_job(s_actor_name[i], PRIO_M, task_fp, (void *)(uintptr_t)i, STACK_SMALL,
                           JOB_FLOAT_POINT, CA_FAULT_ASSERT);
        made = s_h[i] != CA_TASK_HANDLE_NONE;
    }
    made = made && spawn_fn(E91_FP_TASKS, task_spin);
    ca_os_sleep(E91_FP_MS);
    s_fp_run = false;
    const bool out = finish(E91_FP_TASKS + 1U);
    u32 rounds = 0U, bad = 0U, idle = 0U;
    for (u32 i = 0U; i < E91_FP_TASKS; i++) {
        rounds += s_fp_rounds[i];
        bad += s_fp_bad[i];
        if (s_fp_rounds[i] == 0U) idle++;
    }
    const bool ok = made && out && bad == 0U && idle == 0U;
    SAY(name, ok, "3 FP tasks + 1 integer task for %u ms: rounds=%u wrong=%u (want 0)%s",
        E91_FP_MS, rounds, bad, out ? "" : " STUCK");
    return ok;
}
#endif

/* ---- case table, CLI ----------------------------------------------------- */

typedef struct {
    const char *name;
    bool (*run)(const char *name);
    const char *what;
} e91_case_t;

static const e91_case_t s_cases[] = {
    { "pi_basic",   case_pi_basic,     "PI: owner runs at the waiter's priority" },
    { "pi_two_mtx", case_pi_two_mutex, "PI: unlocking one of two mutexes keeps the boost" },
    { "pi_timeout", case_pi_timeout,   "PI: a waiter that times out takes its boost back" },
    { "pi_chain",   case_pi_chain,     "PI: the boost follows a chain of owners" },
    { "pi_setprio", case_pi_setprio,   "PI: set_priority on a boosted owner is kept" },
    { "pi_kill",    case_pi_kill,      "PI: a waiter removed while waiting takes its boost back" },
    { "pi_hopcap",  case_pi_hopcap,    "PI: the chain walk stops at CA_PI_MAX_CHAIN hops" },
    { "rmutex_cnt", case_rmutex_count, "recursive mutex is free only at count zero" },
    { "wake_pend",  case_wake_pending, "wake_up on a task waiting on a semaphore" },
    { "reset_wait", case_reset_waiters,"queue reset with receivers waiting" },
    { "send_front", case_send_front,   "send_front goes ahead of the queued items" },
    { "scr_tmo",    case_scrambled_tmo,"8 timed waiters leave in deadline order" },
    { "cv_signal",  case_cond_signal_one, "condvar: signal wakes exactly one" },
    { "cv_bcast",   case_cond_broadcast,  "condvar: broadcast wakes all" },
    { "cv_relock",  case_cond_tmo_relock, "condvar: a timed-out wait owns the mutex again" },
    { "rw_excl",    case_rwlock_excl,     "rwlock: no torn read, writer not starved" },
    { "coop_spin",  case_coop_spin,       "cooperative spinlock: no lost increment" },
    { "tmr_drift",  case_tmr_drift,       "timer: 10 ms periodic for 2 s, no drift" },
    { "tmr_restart",case_tmr_restart,     "timer: reset re-arms from the reset" },
    { "tmr_stopcb", case_tmr_stop_in_cb,  "timer: stop from its own callback" },
    { "tmr_rearm",  case_tmr_rearm_in_cb, "timer: one-shot re-armed from its own callback" },
    { "tmr_same",   case_tmr_same_tick,   "timer: 8 armed together all fire" },
    { "tmr_period", case_tmr_period,      "timer: set_period applies from the next cycle" },
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    { "fpu_share",  case_fpu_share,       "FPU: preempted FP tasks keep their registers" },
#endif
};
#define E91_CASES  (sizeof(s_cases) / sizeof(s_cases[0]))

static void cmd_list(void) {
    CLI_PUT_N("--- Example 91: kernel semantics bench ---");
    for (u32 i = 0U; i < E91_CASES; i++) {
        CLI_PRINTF_N("  %-11s %s", s_cases[i].name, s_cases[i].what);
    }
}

static void cmd_start(const char *p) {
    p = ca_skip_sp(p);
    const char *only = NULL;
    if (*p != '\0' && ca_strncmp(p, "all", 3) != 0) {
        only = p;
        bool known = false;
        for (u32 i = 0U; i < E91_CASES; i++) {
            if (ca_strncmp(only, s_cases[i].name, ca_strlen(s_cases[i].name)) == 0) known = true;
        }
        if (!known) {
            CLI_PUT_N("e91: unknown case - see 'e91 list'");
            CLI_PUT_N("RUN RESULT : FAIL  (0/1)");
            return;
        }
    }
    if (s_busy) {
        CLI_PUT_N("e91: a pass is running - wait for its result");
        CLI_PUT_N("RUN RESULT : FAIL  (0/1)");
        return;
    }
    s_busy = true;
    u32 runs = 0U, pass = 0U;
    for (u32 i = 0U; i < E91_CASES; i++) {
        const u32 n = ca_strlen(s_cases[i].name);
        if (only != NULL && !(ca_strncmp(only, s_cases[i].name, n) == 0 &&
                              (only[n] == '\0' || only[n] == ' '))) continue;
        runs++;
        if (s_cases[i].run(s_cases[i].name)) pass++;
    }
    s_busy = false;
    CLI_PRINTF_N("RUN RESULT : %s  (%u/%u)", (pass == runs && runs > 0U) ? "OK" : "FAIL",
                 pass, runs);
}

static bool user_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e91       : start [case|all] | list");
        return false;
    }
    if (ca_strncmp(p, "e91", 3) != 0) return false;
    p = ca_skip_sp(p + 3);
    if (ca_strncmp(p, "start", 5) == 0) {
        cmd_start(p + 5);
        return true;
    }
    if (ca_strncmp(p, "list", 4) == 0) {
        cmd_list();
        return true;
    }
    CLI_PUT_N("e91       : start [case|all] | list");
    return true;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

static u32 task_main(void *arg) {
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("example_91: kernel semantics bench, idle - 'e91 list', 'e91 start'");
    return 0;
}

void ca_os_configure(void) {
    (void)ca_sw_timer_init();
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
    ca_os_job("example_91", CA_TH_LEVEL3, task_main, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_ASSERT);
    ca_cli_register(&s_user_cli_node);
    ca_os_run(true);
}

#else  /* no CLI, or too few task slots (F103: MAX_TASK 5) */

PRIVATE_STRING(m91a, "=== Example 91 ===");
PRIVATE_STRING(m91b, "ENABLE_CLI / EN_SW_TIMER / EN_RWLOCK disabled, or MAX_TASK < 16");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m91a);
    PUT_N(&priv_m91b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
