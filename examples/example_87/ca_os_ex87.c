// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * Example 87 - lost wake-up bench: every blocking wait, raced against an ISR
 *
 * The kernel's blocking waits share one shape: under a lock the task puts
 * itself in a wait list, drops the lock, and only THEN calls ca_os_wait(),
 * which marks it BLOCKED. A wake that lands between the unlock and the
 * ca_os_wait() finds the task still RUNNING - ca_task_manager_signal_wake_up()
 * only acts on a BLOCKED task - and is lost. The task then blocks outside any
 * wait list and nobody wakes it again. Found 23/09 on U575 in the stdout
 * drain, where it silenced the whole console.
 *
 * The window is a few cycles wide, so it is not waited for: it is swept. A
 * timer ISR (TIM3, 50 us) produces ONE item and produces the next only after
 * the consumer has taken it. The consumer burns a random time after every
 * item, so the next tick lands at a different point of its path each round,
 * the window included. Because the critical sections mask with PRIMASK, a tick
 * that expires while the consumer holds the lock fires exactly at the unlock:
 * straight into the window.
 *
 * Ping-pong turns a lost wake from a delay into a hang - the ISR waits for an
 * ack that never comes - and the ISR itself watches for it. A consumer that
 * stays BLOCKED for STALL_MS while its primitive has work for it, or while it
 * is in no wait list at all, is counted, classified and woken again so the
 * round can go on:
 *
 *   lost_in_list  BLOCKED, in the wait list, work pending: the item arrived
 *                 between the "empty?" check and the enqueue (queue.c does
 *                 the two under separate locks)
 *   lost_off_list BLOCKED, NOT in the list, work pending: the wake came
 *                 between unlock and ca_os_wait() and was dropped
 *   orphan        BLOCKED, in no list, nothing pending: unlinked from the list
 *                 by someone else (the timed paths of queue.c enqueue the task
 *                 again on every retry, and enqueueing a task that is already
 *                 linked truncates the list behind it)
 *
 * Phases (one primitive each, consumer always waiting WITHOUT a timeout):
 *   queue qheap qsend notify event stream message mixed
 *   mv_queue mv_event mv_stream mv_msg mv_mem
 * 'mixed' puts a timed and an untimed receiver on the same queue.
 *
 * mv_* race the same primitive while the waiter CHANGES INDEX. A thid is a
 * position in the task list and removing a task swaps the last one into the
 * freed slot, rewriting its index. A primitive that remembers its waiter by
 * index (event waiters[], stream/message waiting_task) then wakes the wrong
 * position. The consumer is created last, above EX87_PADS pad tasks; each pad
 * that returns moves it down one index while it waits. mv_queue is the
 * control: wait lists hold context pointers and do not care.
 *
 * mv_mem is not a wake-up race: it moves a task that OWNS heap blocks. The
 * owner allocates EX87_MEM_BLOCKS blocks, and after every move frees one and
 * allocates another. It must keep num_alloc and its usage through the moves,
 * and when it returns - without freeing - the kernel's cleanup must give the
 * heap back to the byte. Ownership recorded by index lost the blocks of a task
 * that had moved (leak) and freed those of whichever task took its old index.
 *
 * Timed waits (since 27/09 a timed waiter sits in the wait list like an
 * untimed one, SLEEP until its deadline, instead of polling off the list):
 *   fair  two timed consumers on one queue, fed by a task: items alternate
 *         (FIFO), none is lost, and each reaches a consumer within a few ms.
 *         Polling gave 107 items to one and 15 to the other (ex4) and up to
 *         10 ms of delay.
 *   tmo   timed receive on a queue nobody feeds, T = 1..20 ms: never back
 *         before T, never much after it.
 *   kill  a waiter, timed or untimed, is removed while in the list, with an
 *         untimed one behind it. The removed context must leave the list,
 *         and the one behind must get the next item. It stayed linked: the
 *         list pointed into a freed context, reused by the next task created.
 *
 * The console thread is NOT a phase. It is the channel this bench answers on,
 * and racing it meant driving the CLI's own dispatch (hook slots, inline
 * fallback) instead of its wait. Its wait is ca_queue_static_receive(..., 0)
 * - the 'queue' phase covers it.
 *
 *   e87 start [phase|all] [sec]   one pass, ends with RUN RESULT
 *
 * No wiring. TIM3 is used only as an interrupt source.
 *
 * Optional load: on the Nucleo-144 boards with Ethernet (H753, H563, F767) the
 * net stack comes up with DHCP (MAC 02:CA:05:00:00:87) and answers ARP and
 * ICMP. It is not a phase: it adds ETH interrupts, a busy 'net' task and the
 * ARP/DHCP service tasks, e.g. with a ping flood running while a pass goes
 * (examples/example_87/stress_peer.py). Nothing in the pass depends on it.
 */

#include "caos.h"
#include "net_proto.h"

#if EN_TIMER && EN_EVENT && EN_NOTIFY && EN_STREAM && EN_MESSAGE && ENABLE_CLI && (MAX_TASK >= 20)

#define EX87_TIM            3U          /* TIM3: present on every family */
#define EX87_PERIOD_US      50U         /* ISR period: one item per tick at most */
/* 'e87 lat' period: prime, so not commensurate with the tick. At 50 us the
 * phase TIM3/tick stays fixed for a whole window and the max depends on which
 * point of the switch it happens to hit; at 53 us it sweeps every phase. */
#define EX87_LAT_PERIOD_US  53U
#define EX87_STALL_MS       100U        /* BLOCKED with work pending this long = lost */
#define EX87_SEC_DEFAULT    3U
#define EX87_BIT            0x01U
#define EX87_LIST_WALK_MAX  32U         /* a corrupted wait list may loop on itself */
#define EX87_PADS           8U          /* moves per mv_* phase: one per pad task */
#define EX87_MEM_BLOCKS     4U          /* mv_mem: heap blocks the moved task owns */
#define EX87_MEM_SIZE       48U
#define EX87_FAIR_MS        2U          /* fair: one item every 2 ms */
#define EX87_FAIR_LAT_MS    3U          /* fair: send -> receive at most this */
#define EX87_TMO_MAX_MS     20U         /* tmo: T cycles through 1..20 ms */
#define EX87_TMO_SLACK_MS   2U          /* tmo: back at most this after T */
#define EX87_KILL_ROUNDS    10U         /* kill: rounds per second */

#if EN_ETH && EN_NET && (MCU_ID == 753 || MCU_ID == 563 || MCU_ID == 767)
#define EX87_NET            1
#else
#define EX87_NET            0
#endif

enum {
    PH_NONE = 0,
    PH_QUEUE, PH_QHEAP, PH_QSEND, PH_NOTIFY, PH_EVENT,
    PH_STREAM, PH_MESSAGE, PH_MIXED,
    PH_MV_QUEUE, PH_MV_EVENT, PH_MV_STREAM, PH_MV_MESSAGE, PH_MV_MEM,
    PH_FAIR, PH_TMO, PH_KILL,
    PH_COUNT
};

static const char *const s_ph_name[PH_COUNT] = {
    "none", "queue", "qheap", "qsend", "notify", "event",
    "stream", "message", "mixed",
    "mv_queue", "mv_event", "mv_stream", "mv_msg", "mv_mem",
    "fair", "tmo", "kill"
};

/* mv_* phases race the same primitive while the consumer CHANGES INDEX. */
static bool is_mv(u8 ph) {
    return ph >= PH_MV_QUEUE && ph <= PH_MV_MEM;
}

static u8 base_of(u8 ph) {
    switch (ph) {
    case PH_MV_QUEUE:   return PH_QUEUE;
    case PH_MV_EVENT:   return PH_EVENT;
    case PH_MV_STREAM:  return PH_STREAM;
    case PH_MV_MESSAGE: return PH_MESSAGE;
    default:            return ph;
    }
}

/* ---- the primitives under test ----------------------------------------- */

static CAQUEUE_STATIC_DEFINE(s_q,  4, sizeof(u32));    /* queue          */
static CAQUEUE_STATIC_DEFINE(s_q1, 1, sizeof(u32));    /* qsend: kept full */
static CAQUEUE_STATIC_DEFINE(s_qm, 4, sizeof(u32));    /* mixed          */
static CAQUEUE_STATIC_DEFINE(s_qf, 4, sizeof(u32));    /* fair           */
static CAQUEUE_STATIC_DEFINE(s_qt, 1, sizeof(u32));    /* tmo: never fed */
static CAQUEUE_STATIC_DEFINE(s_qk, 1, sizeof(u32));    /* kill           */
static ca_queue_t       s_qh;                             /* qheap          */
static bool          s_qh_ok;
static ca_event_group_t  s_eg;
CA_STREAM_STATIC_DEFINE(s_sb, 32);
CA_MESSAGE_STATIC_DEFINE(s_mb, 64);

/* ---- shared state (ISR <-> tasks) ---------------------------------------
 * Consumers are held by CONTEXT, never by index: a thid is a position in the
 * task list and every console command creates and destroys a task, which
 * reshuffles the positions. ctx->index is read at the moment it is needed. */

static ca_thread_context_t *volatile s_wa;     /* untimed consumer (every phase) */
static ca_thread_context_t *volatile s_wb;     /* timed consumer ('mixed' only)  */
static ca_thread_context_t *volatile s_wc;     /* consumer the ISR serves now    */
static volatile bool s_wc_done;            /* mv_* consumer has returned     */
static volatile bool s_hold;               /* ISR: do not produce (a move)   */
static volatile u32  s_pad_release;        /* pads with rank below it return */
static volatile u32  s_moves;              /* consumer index changes seen    */
static volatile u32  s_mem_step;           /* mv_mem: steps asked of the owner */
static volatile u32  s_mem_done;           /* mv_mem: steps the owner did      */
static volatile u32  s_mem_fail;           /* mv_mem: allocations that failed  */

static volatile u8   s_phase;              /* what the consumers serve       */
static volatile u8   s_isr_phase;          /* what the ISR produces for      */
static volatile bool s_exiting;            /* ISR keeps producing until idle */
static volatile bool s_wa_idle = true;
static volatile bool s_wb_idle = true;

static volatile u32  s_pending;            /* 1 = an item is out, not acked  */
static volatile u32  s_sent;
static volatile u32  s_got;
static volatile u32  s_got_b;
static volatile u32  s_wait_ticks;
static volatile u32  s_stuck_ticks;
static volatile u32  s_lost_in_list;
static volatile u32  s_lost_off_list;
static volatile u32  s_orphan;
static volatile u32  s_late;

static volatile bool s_busy;              /* a pass is running (any phase) */
static u32  s_stall_ticks;
static u32  s_jitter_max;
static bool s_tim_ready;
static u32  s_tim_period_us;                /* period TIM3 was set up with */

/* ---- helpers ------------------------------------------------------------ */

static u32 xorshift(u32 *s) {
    u32 x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* A random pause of up to about one ISR period: it is what moves the next
 * tick across the consumer's path. */
static void jitter(u32 *rng) {
    for (volatile u32 d = xorshift(rng) % s_jitter_max; d > 0U; d--) { }
}

static bool in_wait_list(const ca_wait_queue_t *wq, ca_thread_context_t *t) {
    ca_thread_context_t *c = wq->head;
    for (u32 n = 0U; c != NULL && n < EX87_LIST_WALK_MAX; n++) {
        if (c == t) return true;
        c = ca_scheduler_get_next_wait(c);
    }
    return false;
}

/* ---- ISR side ----------------------------------------------------------- */

static void produce(u8 ph) {
    u32 v = s_sent;
    u8  b = (u8)v;
    switch (ph) {
    case PH_QUEUE:   (void)ca_queue_static_send_isr(&s_q.q, &v);           break;
    case PH_QHEAP:   (void)ca_queue_heap_send_isr(&s_qh, &v);              break;
    case PH_QSEND:   (void)ca_queue_static_receive_isr(&s_q1.q, &v);       break;
    case PH_NOTIFY:  if (s_wc) (void)ca_notify_send_handle_isr(ca_os_task_handle(s_wc), 1U,
                                             CA_NOTIFY_ACTION_INCREMENT);  break;
    case PH_EVENT:   (void)ca_event_set_isr(&s_eg, EX87_BIT);              break;
    case PH_STREAM:  (void)ca_stream_send_isr(&s_sb, &b, 1U);              break;
    case PH_MESSAGE: (void)ca_message_send_isr(&s_mb, &v, sizeof(v));      break;
    case PH_MIXED:   (void)ca_queue_static_send_isr(&s_qm.q, &v);          break;
    default: break;
    }
}

/* Does the primitive hold something the consumer should already have? */
static bool has_work(u8 ph, ca_thread_context_t *w) {
    switch (ph) {
    case PH_QUEUE:   return s_q.q.count > 0U;
    case PH_QHEAP:   return s_qh.count > 0U;
    case PH_QSEND:   return s_q1.q.count < s_q1.q.max_items;
    case PH_NOTIFY:  return w->notify_value != 0U;
    /* The waker consumes the bit ON BEHALF of the waiter (auto_clear) and
     * leaves it in waiters[].result: a bit already handed over is work too. */
    case PH_EVENT:   return ((s_eg.bits & EX87_BIT) != 0U) ||
                            (s_eg.waiters[ca_os_task_slot(w)].result != 0U);
    case PH_STREAM:  return s_sb.count > 0U;
    case PH_MESSAGE: return s_mb.msg_count > 0U;
    case PH_MIXED:   return s_qm.q.count > 0U;
    default:         return false;
    }
}

/* Is the consumer registered where the producer will look for it?
 * notify has no registration at all: every stall there is off-list. */
static bool is_listed(u8 ph, ca_thread_context_t *w) {
    switch (ph) {
    case PH_QUEUE:   return in_wait_list(&s_q.q.wait_recv, w);
    case PH_QHEAP:   return in_wait_list(&s_qh.wait_recv, w);
    case PH_QSEND:   return in_wait_list(&s_q1.q.wait_send, w);
    case PH_EVENT:   return in_wait_list(&s_eg.wait_queue, w);
    /* The waiter is registered by handle (ca_task_handle_t) */
    case PH_STREAM:  return s_sb.waiting_task == ca_os_task_handle(w);
    case PH_MESSAGE: return s_mb.waiting_task == ca_os_task_handle(w);
    case PH_MIXED:   return in_wait_list(&s_qm.q.wait_recv, w);
    default:         return false;
    }
}

/* Take the consumer out of the place where the producer would look for it.
 * Safe here: every task-side access to these lists runs with IRQs masked. */
static void unlink(u8 ph, ca_thread_context_t *w) {
    switch (ph) {
    case PH_QUEUE:   (void)ca_wait_queue_remove(&s_q.q.wait_recv, w);  break;
    case PH_QHEAP:   (void)ca_wait_queue_remove(&s_qh.wait_recv, w);   break;
    case PH_QSEND:   (void)ca_wait_queue_remove(&s_q1.q.wait_send, w); break;
    case PH_EVENT:   (void)ca_wait_queue_remove(&s_eg.wait_queue, w);  break;
    case PH_STREAM:  s_sb.waiting_task = 0xFFFFFFFFU;               break;
    case PH_MESSAGE: s_mb.waiting_task = 0xFFFFFFFFU;               break;
    case PH_MIXED:   (void)ca_wait_queue_remove(&s_qm.q.wait_recv, w); break;
    default: break;
    }
}

static void watch_consumer(u8 ph) {
    ca_thread_context_t *w = s_wc;
    if (w == NULL || w->status != CA_ST_TH_BLOCKED) {
        s_stuck_ticks = 0U;
        return;
    }
    const bool work   = has_work(ph, w);
    const bool listed = is_listed(ph, w);
    if (!work && listed) {          /* waiting as it should */
        s_stuck_ticks = 0U;
        return;
    }
    if (++s_stuck_ticks < s_stall_ticks) return;

    /* Stuck. Count it, then wake it without leaving a stale link behind: take
     * it out of the list first, as a producer would, then signal it. Producing
     * again is not a general cure - for qsend "producing" frees a slot, and on
     * an already empty queue it wakes nobody. */
    s_stuck_ticks = 0U;
    s_wait_ticks  = 0U;
    if (listed) {
        s_lost_in_list++;
        unlink(ph, w);
    } else if (work) {
        s_lost_off_list++;
    } else {
        s_orphan++;
    }
    (void)ca_os_signal_handle(ca_os_task_handle(w));
}

/* ---- IRQ latency probe --------------------------------------------------
 * TIM3 runs at the same NVIC priority as the UART RX, so how late its ISR
 * starts is how late a received byte is served: from the update event to the
 * first line of this callback. CNT restarts at every update, so CNT read on
 * entry IS the latency, exact while it stays under one period (P = 50 us).
 * Past that CNT has wrapped; CYCCNT then gives it: the next event came P after
 * the one the previous entry served, so L = d - P + L_prev, exact even when
 * several updates merged into one entry.
 * At a new maximum, the PC the preempted task resumes at is kept: when the
 * delay came from a section with IRQs masked, it is the instruction right
 * after that section re-enabled them - where to look. 0 = a handler was
 * preempted instead (another ISR, or PendSV). */
#define EX87_LAT_BINS  8U
static const u32 s_lat_edge_us[EX87_LAT_BINS - 1U] = { 1U, 2U, 5U, 10U, 20U, 50U, 87U };
static volatile u32 s_lat_hist[EX87_LAT_BINS];
static volatile u32 s_lat_max;              /* cycles */
static volatile u32 s_lat_pc;
static volatile u32 s_lat_exc;              /* exception preempted at the max */
/* Samples >= 2 us by what TIM3 found itself on top of */
enum { LAT_SRC_THREAD, LAT_SRC_PENDSV, LAT_SRC_SYSTICK, LAT_SRC_SVC,
       LAT_SRC_IRQ, LAT_SRC_OTHER, LAT_SRC_N };
static volatile u32 s_lat_src[LAT_SRC_N];
static volatile u32 s_lat_src_max[LAT_SRC_N];   /* cycles, worst per source */
static volatile u32 s_lat_thread_pc;        /* PC at the worst thread-mode one */
static volatile u32 s_lat_src_irq;          /* last IRQ number seen preempted */
static volatile u32 s_lat_prev_cyc;
static volatile u32 s_lat_prev;
static u32 s_lat_period_cyc;                /* one ISR period in CPU cycles */
static u32 s_lat_cnt_mul;                   /* CNT tick -> CPU cycles, Q16 */
static u32 s_lat_cyc_us;                    /* CPU cycles per us */
static volatile bool s_lat_busy;            /* 'e87 lat' window open */

static void lat_reset(void) {
    s_lat_cyc_us     = SystemCoreClock / 1000000U;
    s_lat_period_cyc = s_lat_cyc_us * s_tim_period_us;
    /* Ticks per period straight from the counter's reload: the timer clock
     * differs from the core clock on some parts (F4/H7 APB timers). */
    s_lat_cnt_mul    = (s_lat_period_cyc << 16) / (TIM3->ARR + 1U);
    for (u32 i = 0U; i < EX87_LAT_BINS; i++) s_lat_hist[i] = 0U;
    s_lat_max = 0U;
    s_lat_pc = 0U;
    s_lat_exc = 0U;
    for (u32 i = 0U; i < LAT_SRC_N; i++) s_lat_src[i] = s_lat_src_max[i] = 0U;
    s_lat_thread_pc = 0U;
    s_lat_src_irq = 0U;
    s_lat_prev_cyc = 0U;
    s_lat_prev = 0U;
}

/* Exception number of the handler TIM3 preempted, 0 = thread mode */
static u32 lat_preempted_exc(void) {
    if (SCB->ICSR & SCB_ICSR_RETTOBASE_Msk) return 0U;
    const u32 shcsr = SCB->SHCSR;
    if (shcsr & SCB_SHCSR_PENDSVACT_Msk)  return 14U;
    if (shcsr & SCB_SHCSR_SYSTICKACT_Msk) return 15U;
    if (shcsr & SCB_SHCSR_SVCALLACT_Msk)  return 11U;
    const u32 self = (__get_IPSR() & 0x1FFU) - 16U;
    for (u32 w = 0U; w < sizeof(NVIC->IABR) / sizeof(NVIC->IABR[0]); w++) {
        u32 act = NVIC->IABR[w];
        if (w == self / 32U) act &= ~(1UL << (self % 32U));
        if (act != 0U) return 16U + w * 32U + (u32)__builtin_ctz(act);
    }
    return 1U;                                  /* a fault or NMI */
}

static void lat_sample(void) {
    const u32 cnt = ca_timer_hw_get_cnt(EX87_TIM);
    const u32 now = DWT->CYCCNT;
    u32 lat = (cnt * s_lat_cnt_mul) >> 16;
    if (s_lat_prev_cyc != 0U) {
        const u32 d = now - s_lat_prev_cyc;
        if (d > s_lat_period_cyc + s_lat_period_cyc / 2U) {
            lat = d - s_lat_period_cyc + s_lat_prev;   /* CNT has wrapped */
        }
    }
    s_lat_prev_cyc = now;
    s_lat_prev     = lat;

    const u32 us = lat / s_lat_cyc_us;
    u32 b = 0U;
    while (b < EX87_LAT_BINS - 1U && us >= s_lat_edge_us[b]) b++;
    s_lat_hist[b]++;
    if (us >= 2U || lat > s_lat_max) {
        const u32 exc = lat_preempted_exc();
        if (us >= 2U) {
            u32 src = LAT_SRC_OTHER;
            if (exc == 0U)       src = LAT_SRC_THREAD;
            else if (exc == 14U) src = LAT_SRC_PENDSV;
            else if (exc == 15U) src = LAT_SRC_SYSTICK;
            else if (exc == 11U) src = LAT_SRC_SVC;
            else if (exc >= 16U) { src = LAT_SRC_IRQ; s_lat_src_irq = exc - 16U; }
            s_lat_src[src]++;
            if (lat > s_lat_src_max[src]) {
                s_lat_src_max[src] = lat;
                if (src == LAT_SRC_THREAD) s_lat_thread_pc = ((u32 *)__get_PSP())[6];
            }
        }
        if (lat > s_lat_max) {
            s_lat_max = lat;
            s_lat_exc = exc;
            s_lat_pc  = (exc == 0U) ? ((u32 *)__get_PSP())[6] : 0U;
        }
    }
}

static void lat_print(const char *name) {
    const u32 ns = (s_lat_max * 1000U) / s_lat_cyc_us;
    CLI_PRINTF_N("e87 %-7s : irq_lat max=%u.%02u us pc=0x%08X  <1:%u <2:%u <5:%u <10:%u <20:%u <50:%u <87:%u >=87:%u",
                 name, ns / 1000U, (ns % 1000U) / 10U, s_lat_pc,
                 s_lat_hist[0], s_lat_hist[1], s_lat_hist[2], s_lat_hist[3],
                 s_lat_hist[4], s_lat_hist[5], s_lat_hist[6], s_lat_hist[7]);
    CLI_PRINTF_N("e87 %-7s : >=2us on: thread=%u pendsv=%u systick=%u svc=%u irq=%u (last %u) other=%u  max exc=%u",
                 name, s_lat_src[LAT_SRC_THREAD], s_lat_src[LAT_SRC_PENDSV],
                 s_lat_src[LAT_SRC_SYSTICK], s_lat_src[LAT_SRC_SVC],
                 s_lat_src[LAT_SRC_IRQ], s_lat_src_irq, s_lat_src[LAT_SRC_OTHER], s_lat_exc);
    /* worst per source: how far below the overall max each one stays */
    u32 ns_src[LAT_SRC_N];
    for (u32 i = 0U; i < LAT_SRC_N; i++) ns_src[i] = (s_lat_src_max[i] * 1000U) / s_lat_cyc_us;
    CLI_PRINTF_N("e87 %-7s : >=2us max: thread=%u.%02u us (pc=0x%08X) pendsv=%u.%02u systick=%u.%02u svc=%u.%02u irq=%u.%02u",
                 name, ns_src[LAT_SRC_THREAD] / 1000U, (ns_src[LAT_SRC_THREAD] % 1000U) / 10U,
                 s_lat_thread_pc,
                 ns_src[LAT_SRC_PENDSV] / 1000U, (ns_src[LAT_SRC_PENDSV] % 1000U) / 10U,
                 ns_src[LAT_SRC_SYSTICK] / 1000U, (ns_src[LAT_SRC_SYSTICK] % 1000U) / 10U,
                 ns_src[LAT_SRC_SVC] / 1000U, (ns_src[LAT_SRC_SVC] % 1000U) / 10U,
                 ns_src[LAT_SRC_IRQ] / 1000U, (ns_src[LAT_SRC_IRQ] % 1000U) / 10U);
}

static void ex87_tick(void) {
    lat_sample();
    if (s_isr_phase == PH_NONE) return;
    /* The primitive is the same whether or not the consumer moves. */
    const u8 ph = base_of(s_isr_phase);

    if (s_exiting) {
        /* Push the consumers out of their wait so they can see the phase is
         * over. The watchdog keeps running: a wake lost now is still lost. */
        produce(ph);
        watch_consumer(ph);
        return;
    }

    if (s_hold) {
        /* A move is in progress: the consumer must be found waiting. */
    } else if (s_pending == 0U) {
        s_pending    = 1U;
        s_wait_ticks = 0U;
        s_sent++;
        produce(ph);
    } else if (++s_wait_ticks >= 2U * s_stall_ticks) {
        /* Outstanding far too long without matching any of the stuck shapes
         * above: a hang all the same, reported on its own. */
        s_late++;
        s_wait_ticks = 0U;
    }
    watch_consumer(ph);
}

/* ---- consumers ---------------------------------------------------------- */

static void served(u32 *rng, volatile u32 *counter) {
    (*counter)++;
    s_pending = 0U;
    jitter(rng);
}

/* One blocking wait on the phase's primitive, always WITHOUT a timeout. */
static bool wait_once(u8 ph) {
    u32 v = 0U;
    u8  buf[16];
    switch (ph) {
    case PH_QUEUE:   return ca_queue_static_receive(&s_q.q, &v, CA_WAIT_FOREVER);
    case PH_QHEAP:   return s_qh_ok && ca_queue_heap_receive(&s_qh, &v, CA_WAIT_FOREVER);
    case PH_QSEND:   return ca_queue_static_send(&s_q1.q, &v, CA_WAIT_FOREVER);
    case PH_NOTIFY:  return ca_notify_wait(true, CA_WAIT_FOREVER) != 0U;
    case PH_EVENT:   return (ca_event_wait_any(&s_eg, EX87_BIT, true, CA_WAIT_FOREVER) & EX87_BIT) != 0U;
    case PH_STREAM:  return ca_stream_receive(&s_sb, buf, sizeof(buf), CA_WAIT_FOREVER) > 0U;
    case PH_MESSAGE: return ca_message_receive(&s_mb, buf, sizeof(buf), CA_WAIT_FOREVER) > 0U;
    case PH_MIXED:   return ca_queue_static_receive(&s_qm.q, &v, CA_WAIT_FOREVER);
    default:         return false;
    }
}

static u32 task_rx(void *arg) {
    (void)arg;
    u32 rng = 0xCA05E087U;
    s_wa = (ca_thread_context_t *)ca_scheduler_get_current_context();
    /* Heap queue allocated here, in a task: the allocator wants an owner. */
    s_qh_ok = ca_queue_heap_init(&s_qh, sizeof(u32), 4U);

    while (1) {
        const u8 ph = s_phase;
        if (ph == PH_NONE || ph >= PH_MV_QUEUE) {   /* mv_* and timed: own tasks */
            s_wa_idle = true;
            ca_os_sleep(2);
            continue;
        }
        s_wa_idle = false;
        if (wait_once(ph)) served(&rng, &s_got);
    }
    return 0;
}

/* 'mixed' only: a TIMED receiver on the same queue as task_rx. It waits in
 * the same list, SLEEP instead of BLOCKED: a wake spent on it must not leave
 * task_rx asleep behind it with an item pending (lost_in_list). */
static u32 task_rx_timed(void *arg) {
    (void)arg;
    u32 rng = 0x5EED0087U;
    u32 v;
    s_wb = (ca_thread_context_t *)ca_scheduler_get_current_context();

    while (1) {
        if (s_phase != PH_MIXED) {
            s_wb_idle = true;
            ca_os_sleep(2);
            continue;
        }
        s_wb_idle = false;
        if (ca_queue_static_receive(&s_qm.q, &v, 3U)) served(&rng, &s_got_b);
    }
    return 0;
}

/* mv_* only: created fresh for each phase, AFTER the pads, so it is the last
 * task in the list. Removing a task swaps the last one into the freed slot:
 * every pad that returns moves this consumer down one index, and it stays
 * last, so the next pad moves it again. It returns when the phase ends. */
static u32 task_rx_moved(void *arg) {
    (void)arg;
    u32 rng = 0x3C0E0087U;
    s_wc = (ca_thread_context_t *)ca_scheduler_get_current_context();

    while (1) {
        const u8 ph = s_phase;
        if (!is_mv(ph)) break;
        if (wait_once(base_of(ph))) served(&rng, &s_got);
    }
    s_wc      = NULL;
    s_wc_done = true;
    return 0;
}

/* mv_mem only: the moved task OWNS heap blocks instead of waiting. After every
 * move it frees one block and allocates another, so both the free path (which
 * decrements the owner's num_alloc) and a fresh allocation run from the new
 * position. It returns WITHOUT freeing: giving them back is the kernel's job. */
static u32 task_mem_owner(void *arg) {
    (void)arg;
    void *blk[EX87_MEM_BLOCKS];
    for (u32 i = 0U; i < EX87_MEM_BLOCKS; i++) {
        blk[i] = ca_memory_allocate(EX87_MEM_SIZE);
        if (blk[i] == NULL) s_mem_fail++;
    }
    s_wc = (ca_thread_context_t *)ca_scheduler_get_current_context();

    u32 done = 0U;
    while (s_phase == PH_MV_MEM) {
        if (s_mem_step != done) {
            const u32 k = done % EX87_MEM_BLOCKS;
            if (blk[k] != NULL) (void)ca_memory_free(blk[k], NULL, false);
            blk[k] = ca_memory_allocate(EX87_MEM_SIZE);
            if (blk[k] == NULL) s_mem_fail++;
            done++;
            s_mem_done = done;
        }
        ca_os_sleep(1);
    }
    s_wc      = NULL;
    s_wc_done = true;
    return 0;
}

/* A place holder below the mv_* consumer. Pads return highest first. */
static u32 task_pad(void *arg) {
    const u32 rank = (u32)(uintptr_t)arg;
    while (s_pad_release <= rank) {
        ca_os_sleep(2);
    }
    return 0;
}

/* ---- orchestration (runs in the task of the 'e87 start' command) -------- */

static bool wait_idle(u8 ph, u32 ms) {
    for (u32 t = 0U; t < ms; t += 5U) {
        if (is_mv(ph)) {
            if (s_wc_done) return true;
        } else if (s_wa_idle && (ph != PH_MIXED || s_wb_idle)) {
            return true;
        }
        ca_os_sleep(5);
    }
    return false;
}

static u32 task_usage(const ca_thread_context_t *t) {
    return ca_memory_get_task_usage((u16)ca_os_task_slot(t));
}

static bool run_mem_phase(u32 sec) {
    ca_timer_stop(EX87_TIM);
    s_isr_phase   = PH_NONE;
    s_moves       = 0U;
    s_mem_step    = 0U;
    s_mem_done    = 0U;
    s_mem_fail    = 0U;
    s_wc          = NULL;
    s_wc_done     = false;
    s_pad_release = 0U;
    const u32 base = ca_memory_get_avail_memory();
    s_phase = PH_MV_MEM;

    /* Pads first, owner last: every pad that returns moves the owner down. */
    bool made = true;
    for (u32 i = 0U; i < EX87_PADS && made; i++) {
        made = ca_os_job("e87_pad", CA_TH_LEVEL3, task_pad, (void *)(uintptr_t)(EX87_PADS - 1U - i),
                         STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD) != CA_TASK_HANDLE_NONE;
    }
    made = made && ca_os_job("e87_mem", CA_TH_LEVEL3, task_mem_owner, NULL,
                             STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD) != CA_TASK_HANDLE_NONE;
    for (u32 t = 0U; made && s_wc == NULL && t < 500U; t += 2U) {
        ca_os_sleep(2);
    }
    ca_thread_context_t *owner = s_wc;
    if (!made || owner == NULL) {
        s_phase       = PH_NONE;
        s_pad_release = EX87_PADS;
        CLI_PRINTF_N("e87 %-7s : could not create the pads/owner  FAIL", s_ph_name[PH_MV_MEM]);
        return false;
    }

    const u32 n_start = owner->num_alloc;
    const u32 u_start = task_usage(owner);

    const u32 slice = (sec * 1000U) / (EX87_PADS + 1U);
    for (u32 m = 0U; m < EX87_PADS; m++) {
        ca_os_sleep(slice);
        const u16 before = owner->index;
        s_pad_release++;
        for (u32 t = 0U; owner->index == before && t < 200U; t++) {
            ca_os_sleep(1);
        }
        if (owner->index != before) s_moves++;
        /* One free + one allocation from the new position. */
        s_mem_step++;
        for (u32 t = 0U; s_mem_done != s_mem_step && t < 200U; t++) {
            ca_os_sleep(1);
        }
    }

    /* Read while the owner is still alive: after the moves it must still be
     * credited with its blocks, whatever its index is now. */
    const u32 n_end = owner->num_alloc;
    const u32 u_end = task_usage(owner);

    s_phase = PH_NONE;
    const bool out = wait_idle(PH_MV_MEM, 1000U);
    s_pad_release = EX87_PADS;

    /* The owner returned holding its blocks: once it is removed, the heap has
     * to be back where it started, to the byte. */
    u32 avail = ca_memory_get_avail_memory();
    for (u32 t = 0U; avail != base && t < 500U; t += 5U) {
        ca_os_sleep(5);
        avail = ca_memory_get_avail_memory();
    }
    const u32 leak = (avail < base) ? (base - avail) : 0U;

    const bool blocks = n_start == EX87_MEM_BLOCKS && n_end == EX87_MEM_BLOCKS;
    const bool usage  = u_start != 0U && u_end == u_start;
    const bool moves  = s_moves == EX87_PADS;
    const bool ok     = out && blocks && usage && moves && leak == 0U && s_mem_fail == 0U;

    CLI_PRINTF_N("e87 %-7s : blocks=%u/%u usage=%u->%u leak=%u fail=%u moves=%u/%u%s  %s",
                 s_ph_name[PH_MV_MEM], n_end, EX87_MEM_BLOCKS, u_start, u_end, leak,
                 s_mem_fail, s_moves, EX87_PADS, out ? "" : " STUCK_AT_EXIT",
                 ok ? "PASS" : "FAIL");
    return ok;
}

/* ---- timed waits: fair / tmo / kill (no ISR, a task is the producer) ---- */

static volatile u32 s_fair_got[2];
static volatile u32 s_fair_lat;             /* worst send -> receive, ticks */
static volatile u32 s_fair_done;

/* fair: a timed consumer. The item is the tick it was sent at. */
static u32 task_fair(void *arg) {
    const u32 who = (u32)(uintptr_t)arg;
    u32 v;
    while (s_phase == PH_FAIR) {
        if (ca_queue_static_receive(&s_qf.q, &v, 20U)) {
            const u32 lat = ca_os_tick() - v;
            if (lat > s_fair_lat) s_fair_lat = lat;
            s_fair_got[who & 1U]++;
        }
    }
    s_fair_done++;
    return 0;
}

static bool run_fair_phase(u32 sec) {
    s_fair_got[0] = s_fair_got[1] = 0U;
    s_fair_lat  = 0U;
    s_fair_done = 0U;
    ca_queue_static_reset(&s_qf.q);
    s_phase = PH_FAIR;

    bool made = true;
    for (u32 i = 0U; i < 2U && made; i++) {
        made = ca_os_job("e87_fair", CA_TH_LEVEL3, task_fair, (void *)(uintptr_t)i, STACK_SMALL,
                         CA_JOB_STD, CA_FAULT_RESTART_THREAD) != CA_TASK_HANDLE_NONE;
    }
    ca_os_sleep(50);                        /* both consumers in the list */

    u32 sent = 0U;
    const u32 n = made ? (sec * 1000U) / EX87_FAIR_MS : 0U;
    for (u32 i = 0U; i < n; i++) {
        const u32 v = ca_os_tick();
        if (ca_queue_static_send(&s_qf.q, &v, 50U)) sent++;
        ca_os_sleep(EX87_FAIR_MS);
    }
    ca_os_sleep(50);                        /* last item delivered */
    s_phase = PH_NONE;
    for (u32 t = 0U; s_fair_done < (made ? 2U : 0U) && t < 500U; t += 5U) {
        ca_os_sleep(5);
    }

    const u32 a = s_fair_got[0], b = s_fair_got[1], got = a + b;
    const u32 lo = (a < b) ? a : b;
    const u32 lat_lim = ca_os_tick_x_ms(EX87_FAIR_LAT_MS);
    const bool out  = s_fair_done == 2U;
    const bool ok   = made && out && sent == n && got == sent && n > 0U &&
                      lo * 100U >= got * 40U && s_fair_lat <= lat_lim;
    CLI_PRINTF_N("e87 %-7s : a=%u b=%u sent=%u lat_max=%u/%u ticks%s%s  %s",
                 s_ph_name[PH_FAIR], a, b, sent, s_fair_lat, lat_lim,
                 made ? "" : " NO_TASKS", out ? "" : " STUCK_AT_EXIT",
                 ok ? "PASS" : "FAIL");
    return ok;
}

/* tmo: runs in the command's own task, on a queue nobody feeds. */
static bool run_tmo_phase(u32 sec) {
    ca_queue_static_reset(&s_qt.q);
    s_phase = PH_TMO;
    const u32 slack = ca_os_tick_x_ms(EX87_TMO_SLACK_MS);
    const u32 end   = ca_os_tick() + ca_os_tick_x_ms(sec * 1000U);
    u32 n = 0U, early = 0U, late = 0U, got = 0U, over_max = 0U;
    u32 v;
    while ((i32)(ca_os_tick() - end) < 0) {
        const u32 t_ms = 1U + (n % EX87_TMO_MAX_MS);
        const u32 need = ca_os_tick_x_ms(t_ms);
        const u32 t0   = ca_os_tick();
        if (ca_queue_static_receive(&s_qt.q, &v, t_ms)) got++;
        const u32 dt = ca_os_tick() - t0;
        if (dt < need) {
            early++;
        } else {
            const u32 over = dt - need;
            if (over > over_max) over_max = over;
            if (over > slack) late++;
        }
        n++;
    }
    s_phase = PH_NONE;
    const bool ok = n >= sec * 50U && early == 0U && late == 0U && got == 0U;
    CLI_PRINTF_N("e87 %-7s : n=%u early=%u late=%u got=%u over_max=%u/%u ticks  %s",
                 s_ph_name[PH_TMO], n, early, late, got, over_max, slack,
                 ok ? "PASS" : "FAIL");
    return ok;
}

static ca_thread_context_t *volatile s_kv;      /* kill: the waiter removed */
static ca_thread_context_t *volatile s_ks;      /* kill: the waiter behind it */
static volatile u32 s_ks_got;

static u32 task_kill_victim(void *arg) {
    const u32 timed = (u32)(uintptr_t)arg;
    u32 v;
    s_kv = (ca_thread_context_t *)ca_scheduler_get_current_context();
    while (1) {
        (void)ca_queue_static_receive(&s_qk.q, &v, timed ? 60000U : CA_WAIT_FOREVER);
    }
    return 0;
}

static u32 task_kill_survivor(void *arg) {
    (void)arg;
    u32 v;
    s_ks = (ca_thread_context_t *)ca_scheduler_get_current_context();
    if (ca_queue_static_receive(&s_qk.q, &v, CA_WAIT_FOREVER)) s_ks_got++;
    return 0;
}

/* Returns when @p t is in the kill queue's wait list, false after 200 ms. */
static bool kill_wait_listed(ca_thread_context_t *volatile *t) {
    for (u32 ms = 0U; ms < 200U; ms += 2U) {
        ca_thread_context_t *c = *t;
        if (c != NULL && in_wait_list(&s_qk.q.wait_recv, c)) return true;
        ca_os_sleep(2);
    }
    return false;
}

static u32 task_nop(void *arg) {
    (void)arg;
    return 0;
}

static bool run_kill_phase(u32 sec) {
    const u32 rounds = sec * EX87_KILL_ROUNDS;
    u32 done = 0U, stale = 0U, lost = 0U, setup = 0U, unlisted = 0U;
    s_phase = PH_KILL;
    for (u32 r = 0U; r < rounds; r++) {
        ca_queue_static_reset(&s_qk.q);
        s_kv = NULL;
        s_ks = NULL;
        const u32 got0 = s_ks_got;
        const bool timed = (r & 1U) == 0U;

        /* Victim first, survivor behind it in the same list. */
        if (ca_os_job("e87_kv", CA_TH_LEVEL3, task_kill_victim, (void *)(uintptr_t)(timed ? 1U : 0U),
                      STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD) == CA_TASK_HANDLE_NONE) {
            setup++;
            break;
        }
        if (!kill_wait_listed(&s_kv)) {
            /* A timed waiter that never enters the list (the polling kernel):
             * nothing to test in this round, and a FAIL of its own. */
            unlisted++;
            if (s_kv != NULL) (void)ca_os_remove_job_handle(ca_os_task_handle(s_kv));
            ca_os_sleep(10);
            continue;
        }
        if (ca_os_job("e87_ks", CA_TH_LEVEL3, task_kill_survivor, NULL,
                      STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD) == CA_TASK_HANDLE_NONE ||
            !kill_wait_listed(&s_ks)) {
            setup++;
            if (s_kv != NULL) (void)ca_os_remove_job_handle(ca_os_task_handle(s_kv));
            break;
        }
        ca_thread_context_t *victim = s_kv;
        (void)ca_os_remove_job_handle(ca_os_task_handle(victim));
        ca_os_sleep(10);                    /* removed and freed by now */

        /* Its context must be out of the list before anyone reuses it. */
        if (in_wait_list(&s_qk.q.wait_recv, victim)) {
            stale++;
            ca_spinlock_lock_irq(&s_qk.q.lock);
            (void)ca_wait_queue_remove(&s_qk.q.wait_recv, victim);
            ca_spinlock_unlock_irq(&s_qk.q.lock);
        }
        /* A new task takes the freed context: a stale link now leads to it. */
        (void)ca_os_job("e87_nop", CA_TH_LEVEL3, task_nop, NULL, STACK_SMALL,
                        CA_JOB_STD, CA_FAULT_RESTART_THREAD);

        const u32 v = r;
        (void)ca_queue_static_send(&s_qk.q, &v, 10U);
        for (u32 t = 0U; s_ks_got == got0 && t < 100U; t += 2U) {
            ca_os_sleep(2);
        }
        if (s_ks_got == got0) {
            lost++;
            if (s_ks != NULL) (void)ca_os_remove_job_handle(ca_os_task_handle(s_ks));
            ca_os_sleep(10);
            ca_spinlock_lock_irq(&s_qk.q.lock);
            ca_wait_queue_init(&s_qk.q.wait_recv);
            ca_spinlock_unlock_irq(&s_qk.q.lock);
        }
        done++;
    }
    ca_queue_static_reset(&s_qk.q);
    s_phase = PH_NONE;
    const bool ok = done == rounds && stale == 0U && lost == 0U && setup == 0U &&
                    unlisted == 0U;
    CLI_PRINTF_N("e87 %-7s : rounds=%u/%u stale=%u lost=%u unlisted=%u%s  %s",
                 s_ph_name[PH_KILL], done, rounds, stale, lost, unlisted,
                 setup ? " SETUP_FAILED" : "", ok ? "PASS" : "FAIL");
    return ok;
}

static bool run_phase(u8 ph, u32 sec) {
    if (ph == PH_MV_MEM) return run_mem_phase(sec);
    if (ph == PH_FAIR)   return run_fair_phase(sec);
    if (ph == PH_TMO)    return run_tmo_phase(sec);
    if (ph == PH_KILL)   return run_kill_phase(sec);
    ca_timer_stop(EX87_TIM);
    s_isr_phase     = PH_NONE;
    s_exiting       = false;
    s_pending       = 0U;
    s_sent          = 0U;
    s_got           = 0U;
    s_got_b         = 0U;
    s_wait_ticks    = 0U;
    s_stuck_ticks   = 0U;
    s_lost_in_list  = 0U;
    s_lost_off_list = 0U;
    s_orphan        = 0U;
    s_late          = 0U;
    s_moves         = 0U;
    s_hold          = false;
    s_wc            = s_wa;

    if (ph == PH_QSEND) {
        /* qsend waits for SPACE: start with the one slot taken. */
        u32 v = 0U;
        (void)ca_queue_static_trysend(&s_q1.q, &v);
    }

    s_wa_idle   = false;
    s_wb_idle   = false;
    s_phase     = ph;

    ca_thread_context_t *moved = NULL;
    if (is_mv(ph)) {
        /* Pads first, consumer last: see task_rx_moved(). */
        s_wc          = NULL;
        s_wc_done     = false;
        s_pad_release = 0U;
        bool made = true;
        for (u32 i = 0U; i < EX87_PADS && made; i++) {
            made = ca_os_job("e87_pad", CA_TH_LEVEL3, task_pad, (void *)(uintptr_t)(EX87_PADS - 1U - i),
                             STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD) != CA_TASK_HANDLE_NONE;
        }
        made = made && ca_os_job("e87_rxm", CA_TH_LEVEL3, task_rx_moved, NULL,
                                 STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD) != CA_TASK_HANDLE_NONE;
        for (u32 t = 0U; made && s_wc == NULL && t < 500U; t += 2U) {
            ca_os_sleep(2);
        }
        moved = s_wc;
        if (!made || moved == NULL) {
            s_phase       = PH_NONE;
            s_pad_release = EX87_PADS;
            CLI_PRINTF_N("e87 %-7s : could not create the pads/consumer  FAIL", s_ph_name[ph]);
            return false;
        }
    }

    s_isr_phase = ph;
    lat_reset();
    ca_timer_start(EX87_TIM);

    if (moved == NULL) {
        ca_os_sleep(sec * 1000U);
    } else {
        /* One move per pad, spread over the phase. Production is held while
         * the pad goes, so the consumer is surely WAITING when its index
         * changes; the next item must then reach it at the new one. */
        const u32 slice = (sec * 1000U) / (EX87_PADS + 1U);
        for (u32 m = 0U; m < EX87_PADS; m++) {
            ca_os_sleep(slice);
            s_hold = true;
            ca_os_sleep(3);
            const u16 before = moved->index;
            s_pad_release++;
            for (u32 t = 0U; moved->index == before && t < 200U; t++) {
                ca_os_sleep(1);
            }
            if (moved->index != before) s_moves++;
            s_hold = false;
        }
        ca_os_sleep(slice);
    }

    s_exiting = true;
    s_phase   = PH_NONE;
    const bool out = wait_idle(ph, 1000U);
    ca_timer_stop(EX87_TIM);
    s_isr_phase = PH_NONE;
    s_exiting   = false;

    s_pad_release = EX87_PADS;              /* no pad left behind */

    const u32 got   = s_got + s_got_b;
    const u32 lost  = s_lost_in_list + s_lost_off_list + s_orphan + s_late;
    const bool enough = got >= (sec * 50U);
    const bool moves  = !is_mv(ph) || s_moves == EX87_PADS;
    const bool ok   = out && enough && moves && lost == 0U && (ph != PH_QHEAP || s_qh_ok);

    CLI_PRINTF_N("e87 %-7s : iter=%u lost_in_list=%u lost_off_list=%u orphan=%u late=%u%s%s%s  %s",
                 s_ph_name[ph], got, s_lost_in_list, s_lost_off_list, s_orphan, s_late,
                 out ? "" : " STUCK_AT_EXIT",
                 enough ? "" : " TOO_FEW_ITER",
                 moves ? "" : " TOO_FEW_MOVES",
                 ok ? "PASS" : "FAIL");
    lat_print(s_ph_name[ph]);
    if (is_mv(ph)) {
        CLI_PRINTF_N("e87 %-7s : moves=%u/%u", s_ph_name[ph], s_moves, EX87_PADS);
    }
    if (ph == PH_MIXED) {
        CLI_PRINTF_N("e87 mixed   : untimed=%u timed=%u", s_got, s_got_b);
    }
    return ok;
}

static u8 phase_from_name(const char *p) {
    for (u8 i = 1U; i < PH_COUNT; i++) {
        const u32 n = ca_strlen(s_ph_name[i]);
        if (ca_strncmp(p, s_ph_name[i], n) == 0 && (p[n] == ' ' || p[n] == '\0'
                                                    || p[n] == '\r' || p[n] == '\n')) {
            return i;
        }
    }
    return PH_NONE;
}

/* TIM3 at period_us, re-armed only when the period changes */
static bool tim_setup(u32 period_us) {
    if (s_tim_ready && s_tim_period_us == period_us) return true;
    if (s_tim_ready) (void)ca_timer_deinit(EX87_TIM);
    s_tim_ready = ca_timer_init_periodic(EX87_TIM, period_us, ex87_tick);
    s_tim_period_us = s_tim_ready ? period_us : 0U;
    return s_tim_ready;
}

/* 'churn': short-lived tasks created and dying for the whole window, so the
 * probe sees what task creation and the remove pass in PendSV cost the
 * interrupts. Built-in CLI commands run in the cli task and create none. */
static volatile u32 s_churn_made;           /* children created */
static volatile u32 s_churn_fail;           /* creations refused */

static u32 churn_child(void *arg) {
    (void)arg;
    u8 *m = (u8 *)ca_memory_allocate(64U);
    if (m != NULL) {
        m[0] = 1U;
        ca_memory_free(m, NULL, false);
    }
    return 0U;
}

static u32 churn_task(void *arg) {
    (void)arg;
    while (s_lat_busy) {
        if (ca_os_job("e87_ch", CA_TH_LEVEL3, churn_child, NULL, STACK_SMALL,
                      CA_JOB_STD, CA_FAULT_ASSERT) != CA_TASK_HANDLE_NONE) {
            s_churn_made++;
        } else {
            s_churn_fail++;
        }
        ca_os_sleep(1U);
    }
    return 0U;
}

/* 'heap': one task keeps the heap fragmented for the whole window - a table
 * of live blocks of mixed sizes, one freed and one allocated at random per
 * step, no sleep. ca_memory_allocate walks the chain with IRQs masked: this
 * is what that walk costs the interrupts. Its blocks go back at the end. */
#define EX87_HEAP_SLOTS  64U
static volatile u32 s_heap_ops;             /* allocations done */
static volatile u32 s_heap_fail;            /* allocations refused */

static void *s_heap_slot[EX87_HEAP_SLOTS];  /* static: STACK_SMALL is tight */

static u32 heap_task(void *arg) {
    (void)arg;
    void **slot = s_heap_slot;
    u32 rng = 0x12345678U;
    while (s_lat_busy) {
        rng = rng * 1664525U + 1013904223U;
        const u32 i = (rng >> 8) % EX87_HEAP_SLOTS;
        if (slot[i] != NULL) (void)ca_memory_free(slot[i], NULL, false);
        slot[i] = ca_memory_allocate(16U + ((rng >> 16) % 497U));
        if (slot[i] != NULL) s_heap_ops++; else s_heap_fail++;
    }
    for (u32 i = 0U; i < EX87_HEAP_SLOTS; i++) {
        if (slot[i] != NULL) (void)ca_memory_free(slot[i], NULL, false);
        slot[i] = NULL;
    }
    return 0U;
}

/* 'spin': a task always ready that touches nothing - to tell what the heap
 * load costs the PendSV from what any busy task does */
static volatile u32 s_spin_loops;

static u32 spin_task(void *arg) {
    (void)arg;
    while (s_lat_busy) s_spin_loops++;
    return 0U;
}

/* 'e87 lat [sec] [churn|heap|spin]': the probe alone, no phase. 'churn' adds
 * tasks that are born and die during the window (see churn_task), 'heap'
 * a task that keeps the heap fragmented (see heap_task). */
static void cmd_lat(const char *p) {
    u32 sec = EX87_SEC_DEFAULT;
    p = ca_skip_sp(p);
    if (*p >= '0' && *p <= '9') {
        const i32 s = ca_strtol(p, 10);
        if (s >= 1 && s <= 600) sec = (u32)s;
        while (*p >= '0' && *p <= '9') p++;
        p = ca_skip_sp(p);
    }
    const bool churn = (ca_strncmp(p, "churn", 5) == 0);
    const bool heap  = (ca_strncmp(p, "heap", 4) == 0);
    const bool spin  = (ca_strncmp(p, "spin", 4) == 0);
    /* TIM3 and the probe counters are the phases' own: a window opened
     * during a pass would reset them and stop the timer under it, leaving
     * the pass without its ISR (STUCK_AT_EXIT, TOO_FEW_MOVES). */
    if (s_busy || s_isr_phase != PH_NONE || s_lat_busy) {
        CLI_PUT_N("e87 lat: a pass or a window is running - wait for its result");
        return;
    }
    if (!tim_setup(EX87_LAT_PERIOD_US)) {
        CLI_PUT_N("e87: TIM3 init failed");
        return;
    }
    s_lat_busy = true;
    lat_reset();
    s_churn_made = 0U;
    s_churn_fail = 0U;
    s_heap_ops   = 0U;
    s_heap_fail  = 0U;
    s_spin_loops = 0U;
    if (spin && ca_os_job("e87_spin", CA_TH_LEVEL3, spin_task, NULL, STACK_SMALL,
                          CA_JOB_STD, CA_FAULT_ASSERT) == CA_TASK_HANDLE_NONE) {
        CLI_PUT_N("e87 lat: spin task not created");
    }
    if (heap && ca_os_job("e87_heap", CA_TH_LEVEL3, heap_task, NULL, STACK_SMALL,
                          CA_JOB_STD, CA_FAULT_ASSERT) == CA_TASK_HANDLE_NONE) {
        CLI_PUT_N("e87 lat: heap task not created");
    }
    if (churn && ca_os_job("e87_churn", CA_TH_LEVEL3, churn_task, NULL, STACK_SMALL,
                           CA_JOB_STD, CA_FAULT_ASSERT) == CA_TASK_HANDLE_NONE) {
        CLI_PUT_N("e87 lat: churn task not created");
    }
    ca_timer_start(EX87_TIM);
    ca_os_sleep(sec * 1000U);
    ca_timer_stop(EX87_TIM);
    s_lat_busy = false;
    lat_print("lat");
    if (churn) {
        CLI_PRINTF_N("e87 lat     : churn tasks created=%u refused=%u", s_churn_made, s_churn_fail);
    }
    if (heap) {
        CLI_PRINTF_N("e87 lat     : heap allocations=%u refused=%u", s_heap_ops, s_heap_fail);
    }
}

static void cmd_start(const char *p) {
    u8  only = PH_NONE;
    u32 sec  = EX87_SEC_DEFAULT;

    p = ca_skip_sp(p);
    if (ca_strncmp(p, "all", 3) == 0) {
        p = ca_skip_sp(p + 3);
    } else if (*p >= 'a' && *p <= 'z') {
        only = phase_from_name(p);
        if (only == PH_NONE) {
            CLI_PUT_N("e87: unknown phase - see 'e87 info'");
            return;
        }
        p = ca_skip_sp(p + ca_strlen(s_ph_name[only]));
    }
    if (*p >= '0' && *p <= '9') {
        const i32 s = ca_strtol(p, 10);
        if (s >= 1 && s <= 60) sec = (u32)s;
    }

    /* s_busy, not only s_isr_phase: fair/tmo/kill run without the ISR, and a
     * second 'e87 start' sent meanwhile (the CLI runs it inline when cli_cmd
     * is taken) ran the same phase twice - two producers on a full queue
     * whose consumers the first had already ended, the console busy ~2 min. */
    if (s_busy || s_lat_busy || s_isr_phase != PH_NONE) {
        CLI_PUT_N("e87: a pass or an 'e87 lat' window is running - wait for its result");
        CLI_PUT_N("RUN RESULT : FAIL  (0/1)");
        return;
    }
    /* After the busy check: re-arming TIM3 under a running window would
     * change its period mid-measure */
    if (!tim_setup(EX87_PERIOD_US)) {
        CLI_PUT_N("e87: TIM3 init failed");
        CLI_PUT_N("RUN RESULT : FAIL  (0/1)");
        return;
    }
    s_busy = true;

    u32 runs = 0U, pass = 0U;
    for (u8 ph = 1U; ph < PH_COUNT; ph++) {
        if (only != PH_NONE && ph != only) continue;
        runs++;
        if (run_phase(ph, sec)) pass++;
    }
    s_busy = false;
    CLI_PRINTF_N("RUN RESULT : %s  (%u/%u)", (pass == runs) ? "OK" : "FAIL", pass, runs);
}

static void cmd_info(void) {
    CLI_PUT_N("--- Example 87: lost wake-up bench ---");
    CLI_PRINTF_N("  isr      : TIM3 every %u us, one item out at a time", EX87_PERIOD_US);
    CLI_PRINTF_N("  stall    : BLOCKED with work pending for %u ms", EX87_STALL_MS);
    CLI_PRINTF_N("  jitter   : 0..%u spin loops after every item", s_jitter_max);
    CLI_PUT_N("  phases   : queue qheap qsend notify event stream message mixed");
    CLI_PUT_N("             mv_queue mv_event mv_stream mv_msg (consumer moves index)");
    CLI_PUT_N("             mv_mem (a heap owner moves index, then exits holding it)");
    CLI_PUT_N("             fair tmo kill (timed waits: FIFO order, deadline, removal)");
    CLI_PRINTF_N("  qheap    : %s", s_qh_ok ? "allocated" : "ALLOCATION FAILED");
}

static bool user_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e87       : start [phase|all] [sec] | lat [sec] [churn|heap|spin] | info");
        return false;
    }
    if (ca_strncmp(p, "e87", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "start", 5) == 0) {
        cmd_start(p + 5);
        return true;
    }
    if (ca_strncmp(p, "lat", 3) == 0) {
        cmd_lat(p + 3);
        return true;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        cmd_info();
        return true;
    }
    CLI_PUT_N("e87       : start [phase|all] [sec] | info");
    return true;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ---- optional network load (see the header) ---------------------------- */

#if EX87_NET
/* Pre-scheduler: ca_stdout_* needs a task context, so PUT_N. */
PRIVATE_STRING(m87n, "example_87: ca_net_init FAILED - no network load");
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x87U } };

static const ca_net_cfg_t s_net_cfg = {
    .ip   = { .addr = 0U },                 /* 0.0.0.0 until DHCP binds */
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x87U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

static ca_eth_cfg_t s_eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x87U } },
    .link_cb  = NULL,
};

/* Nucleo-144 RMII pins, AF11 (as ex67). */
static void eth_gpio_init(void) {
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
#if MCU_ID == 563
        { CA_GPIO_PORT_B, 15U },   /* RMII_TXD1: PB15 on NUCLEO-H563ZI */
#else
        { CA_GPIO_PORT_B, 13U },
#endif
    };
    for (u32 i = 0U; i < sizeof(pins) / sizeof(pins[0]); i++) {
        ca_gpio_configure(pins[i].port, pins[i].pin, CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
                          CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, 11U);
    }
}

/* DHCP (port 68) - as ex35 */
static void dhcp_udp_handler(u16 buf_idx) {
    ca_net_buf_t *b = ca_net_buf_ptr(buf_idx);
    u8 *frame = b->data + b->data_off;
    const u16 l4_off = b->meta.rx.l4_off;
    ca_dhcp_service_rx_handler((const ca_udp_frame_t *)(frame + l4_off),
                               (u16)(b->data_len - l4_off));
    ca_net_buf_free(buf_idx);
}

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns) {
    (void)mask; (void)gw; (void)dns;
    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_stdout_printf_n("example_87: DHCP bound IP=%u.%u.%u.%u",
                       ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    ca_arp_send_garp();
}

static void on_dhcp_expire(void) {
    ca_stdout_put_n("example_87: DHCP lease expired - renewing");
}

static u32 task_net(void *arg) {
    (void)arg;
    ca_net_proto_init(&s_net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    ca_eth_link_poll();
    ca_os_sleep(200);
    ca_dhcp_service_net_ready();            /* the DHCP task waits for this */
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

static void net_configure(void) {
    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&s_eth_cfg))) {
        PUT_N(&priv_m87n);
        return;
    }
    const ca_ipv4_t zero = { .addr = 0U };
    ca_arp_service_init(s_mac, zero);
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
}
#endif /* EX87_NET */

/* ---- entry points ------------------------------------------------------- */

static u32 task_main(void *arg) {
    (void)arg;
    ca_os_sleep(500);
    ca_stdout_put_n("example_87: lost wake-up bench, idle - 'e87 start' runs one pass");
#if EX87_NET
    ca_stdout_put_n("example_87: net load on, IP from DHCP - see stress_peer.py");
#endif
    ca_stdout_put_n("example_87: ready");
    while (1) {
        ca_os_sleep(1000);
    }
    return 0;
}

void ca_os_configure(void) {
    ca_event_init(&s_eg);

    /* About one ISR period of spinning at most: a volatile countdown costs
     * roughly 4 cycles per step on these cores. */
    s_jitter_max  = (SystemCoreClock / 1000000U) * EX87_PERIOD_US / 4U;
    if (s_jitter_max == 0U) s_jitter_max = 1U;
    s_stall_ticks = (EX87_STALL_MS * 1000U) / EX87_PERIOD_US;

    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    ca_cli_init(2048);
    ca_cli_set_sender(STDOUT);
    ca_cli_create_thread();
#if EX87_NET
    net_configure();
#endif
}

void ca_os_main(void) {
    ca_timer_init();
    ca_os_job("example_87", CA_TH_LEVEL3, task_main,     NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("e87_rx",     CA_TH_LEVEL3, task_rx,       NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("e87_rxt",    CA_TH_LEVEL3, task_rx_timed, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
#if EX87_NET
    ca_arp_service_start();
    ca_dhcp_service_start();
    /* STACK_SMALL and no link-monitor task, as the PSP area has to hold the 9
     * tasks of an mv_* phase on top of these: with 'net' at STACK_MEDIUM plus
     * an 'eth_mon' the H753's 104 KB were full and the pads could not be
     * created. The link poll only feeds link_cb, which is NULL here. */
    ca_os_job("net",        CA_TH_LEVEL4, task_net,      NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
#endif
    ca_cli_register(&s_user_cli_node);
    ca_os_run(true);
}

#else  /* a flag missing, or too few task slots (F103: MAX_TASK 5) */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m87a, "=== Example 87 ===");
#if (MAX_TASK < 20)
PRIVATE_STRING(m87b, "MAX_TASK too small for the bench (needs 20 task slots)");
#else
PRIVATE_STRING(m87b, "EN_TIMER / EN_EVENT / EN_NOTIFY / EN_STREAM / EN_MESSAGE / ENABLE_CLI disabled");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m87a);
    PUT_N(&priv_m87b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
