// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

#if EN_MESSAGE

/*
 * Message Buffer usage example
 *
 * The Message Buffer is a buffer for messages with framing.
 * Unlike the Stream Buffer (continuous stream of bytes),
 * the Message Buffer preserves message boundaries:
 * - Each send writes [length][payload] as an atomic unit
 * - Each receive reads a complete message or nothing
 *
 * In this example:
 * - A writer task sends structured messages
 * - A reader task receives the messages
 * - A monitor task shows the buffer status
 * - The CLI allows controlling prints and viewing statistics
 *
 * Features demonstrated:
 * - ca_message_init() / ca_message_destroy() (heap allocation)
 * - ca_message_send() / ca_message_receive() with timeout
 * - ca_message_count() - number of messages in queue
 * - ca_message_peek_size() - size of next message
 * - ca_message_space_available() - free space
 * - ca_message_reset() - resets buffer
 *
 * CLI COMMANDS:
 * - start: enables task prints
 * - stop: disables task prints
 * - show: shows message buffer statistics
 * - clear: empties the message buffer
 * - help: list available commands
 */

/* Maximum payload size per message */
#define MSG_PAYLOAD_SIZE    64U

/* Buffer size: payload + header overhead (2 bytes per message) */
/* With 10 maximum messages: 10 * (2 + 64) = 660 bytes */
#define MSG_BUFFER_SIZE     660U

/* Maximum number of messages in the buffer */
#define MSG_MAX_COUNT       10U

/* Message structure */
typedef struct {
    u32 id;             /* Sender ID */
    u32 sequence;       /* Sequence number */
    char payload[MSG_PAYLOAD_SIZE];  /* Data (string) */
} Message_t;

/* Message buffer (heap allocation) */
static ca_message_buffer_t msg_buffer;

static u32 running;
static u32 messages_sent;
static u32 messages_received;
static u32 writer_cycles;
static u32 reader_cycles;
static volatile u8 print_enabled;  // 0 = prints disabled, 1 = enabled

/* =============================================================================
 * TASK: Writer - sends structured messages
 * ============================================================================= */

static u32 task_writer(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    if (print_enabled) {
        ca_stdout_printf_n("WRITER started: idx=%u p1=%u", idx, p1);
    }

    Message_t msg;
    u32 sequence = 0;

    while (running) {
        // Prepare structured message
        msg.id = 1;  // Writer ID
        msg.sequence = sequence;
        ca_snprintf(msg.payload, MSG_PAYLOAD_SIZE, "Hello from Writer #%u", sequence);

        // Send message (500ms timeout)
        bool ok = ca_message_send(&msg_buffer, &msg, sizeof(Message_t), 500);

        if (ok) {
            messages_sent++;
            writer_cycles++;
            sequence++;

            if (print_enabled) {
                ca_stdout_printf_n("WRITER: sent msg seq=%u (size=%u)", 
                                   sequence - 1, sizeof(Message_t));
            }
        } else {
            if (print_enabled) {
                ca_stdout_printf_n("WRITER: timeout (buffer full?)");
            }
        }

        // Wait before next send
        ca_os_sleep(300);
    }

    return 0;
}

/* =============================================================================
 * TASK: Reader - receives structured messages
 * ============================================================================= */

static u32 task_reader(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    if (print_enabled) {
        ca_stdout_printf_n("READER started: idx=%u p1=%u", idx, p1);
    }

    Message_t msg;
    u16 received_len;

    while (running) {
        // Peek the size of the next message
        u16 peek_size = ca_message_peek_size(&msg_buffer);

        if (peek_size > 0) {
            if (print_enabled) {
                ca_stdout_printf_n("READER: peek size=%u", peek_size);
            }
        }

        // Receive message (200ms timeout)
        received_len = ca_message_receive(&msg_buffer, &msg, sizeof(Message_t), 200);

        if (received_len > 0) {
            messages_received++;
            reader_cycles++;

            if (print_enabled) {
                ca_stdout_printf_n("READER: recv msg from ID=%u seq=%u payload='%s'",
                                   msg.id, msg.sequence, msg.payload);
            }
        }
    }

    return 0;
}

/* =============================================================================
 * TASK: Monitor - shows message buffer statistics
 * ============================================================================= */

static u32 task_monitor(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    // Wait for the system to stabilize
    ca_os_sleep(1000);

    if (print_enabled) {
        ca_stdout_printf_n("MONITOR started: idx=%u p1=%u", idx, p1);
    }

    while (running) {
        u16 msg_count = ca_message_count(&msg_buffer);
        u32 free_space = ca_message_space_available(&msg_buffer);
        bool is_empty = ca_message_is_empty(&msg_buffer);
        u16 next_size = ca_message_peek_size(&msg_buffer);

        if (print_enabled) {
            ca_stdout_printf_n("=== MESSAGE BUFFER STATUS ===");
            ca_stdout_printf_n("Messages in queue : %u / %u", msg_count, MSG_MAX_COUNT);
            ca_stdout_printf_n("Free space        : %u / %u bytes", free_space, MSG_BUFFER_SIZE);
            ca_stdout_printf_n("Empty             : %s", is_empty ? "YES" : "NO ");
            ca_stdout_printf_n("Next message size : %u bytes", next_size);
            ca_stdout_printf_n("Total sent        : %u messages (%u cycles)", messages_sent, writer_cycles);
            ca_stdout_printf_n("Total recv        : %u messages (%u cycles)", messages_received, reader_cycles);
            ca_stdout_printf_n("=============================");
        }

        // Print every 3 seconds
        ca_os_sleep(3000);
    }

    return 0;
}

/* =============================================================================
 * CLI HOOK - user commands for message buffer
 * ============================================================================= */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("start | stop | show | clear");
        return false;
    }
    if (ca_strncmp(p, "start", 5) == 0) {
        print_enabled = 1;
        CLI_PUT_N("Print ENABLED");
        return true;
    }
    if (ca_strncmp(p, "stop", 4) == 0) {
        print_enabled = 0;
        CLI_PUT_N("Print DISABLED");
        return true;
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        CLI_PUT_N("--- Message Buffer Statistics:");
        CLI_PRINTF_N("   Messages sent    : %u", messages_sent);
        CLI_PRINTF_N("   Messages received: %u", messages_received);
        CLI_PRINTF_N("   Writer cycles    : %u", writer_cycles);
        CLI_PRINTF_N("   Reader cycles    : %u", reader_cycles);
        CLI_PRINTF_N("   Messages in queue: %u", ca_message_count(&msg_buffer));
        CLI_PRINTF_N("   Free space       : %u bytes", ca_message_space_available(&msg_buffer));
        CLI_PRINTF_N("   Print status     : %s", print_enabled ? "ENABLED" : "DISABLED");
        return true;
    }
    if (ca_strncmp(p, "clear", 5) == 0) {
        ca_message_reset(&msg_buffer);
        CLI_PUT_N("Message buffer cleared!");
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };



/* ============================================================================
 * 'twait [sec]': timed wait check on the message buffer (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task sends one message after 1..8 ms; the waiter, with a
 *             20 ms timeout, must get it and be running within TW_LAT_MS.
 *   timeout : nobody sends; T = 1..20 ms, never back before T,
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
CA_MESSAGE_STATIC_DEFINE(tw_obj, 64);
static void tw_init(void) {  }
static void tw_hold(void) {  }
static void tw_give(void) { const u32 v = 1U; (void)ca_message_send(&tw_obj, &v, sizeof(v), CA_NO_WAIT_TIME); }
static bool tw_wait(u32 ms) { u32 v; return ca_message_receive(&tw_obj, &v, sizeof(v), ms) > 0U; }
static void tw_after(void) {  }
static void tw_drain(void) { ca_message_reset(&tw_obj); }

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
            CLI_PUT_N("TW message : peer task not created  FAIL");
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
    CLI_PRINTF_N("TW message : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the message buffer: latency + timeout");
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

/* =============================================================================
 * ca_os_configure - module initialization
 * ============================================================================= */

void ca_os_configure(void) {
    /* Initialize temperature sensor */

    /* Log system */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    /* CLI */
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )

    /* USB CDC */
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}

/* =============================================================================
 * ca_os_main - entry point
 * ============================================================================= */

void ca_os_main(void) {
    EVAL_MESSAGE(
        running = 1;
        messages_sent = 0;
        messages_received = 0;
        writer_cycles = 0;
        reader_cycles = 0;
        print_enabled = 0;

        /* Initialize message buffer with heap allocation */
        if (!ca_message_init(&msg_buffer, MSG_BUFFER_SIZE)) {
            while (1) { __NOP(); }
        }

        ca_os_job("writer",  CA_TH_LEVEL2, task_writer,  NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
        ca_os_job("reader",  CA_TH_LEVEL2, task_reader,  NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
        ca_os_job("monitor", CA_TH_LEVEL1, task_monitor, NULL, STACK_SMALL,  CA_JOB_STD, CA_FAULT_ASSERT);

        EVAL_CLI( ca_cli_register(&s_user_cli_node); )
    )
    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    ca_os_run(true);
}

#else /* EN_MESSAGE */
/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m15a, "=== Example 15 ===");
PRIVATE_STRING(m15b, "EN_MESSAGE ==0, skipping message buffer example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m15a);
    PUT_N(&priv_m15b);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_MESSAGE */
/* =============================================================================
 * IMPORTANT NOTES
 * =============================================================================
 *
 * 1. Message Buffer vs Stream Buffer:
 *    - Message: distinct messages with preserved boundaries
 *    - Stream: continuous stream of bytes (like a pipe)
 *
 * 2. Heap Allocation:
 *    - ca_message_init(): buffer dynamically allocated
 *    - ca_message_destroy(): frees memory (not used in this example)
 *
 * 3. Message Structure:
 *    - id: sender identifier
 *    - sequence: progressive number
 *    - payload: application data (string)
 *
 * 4. Overhead:
 *    - Each message has a 2-byte header for length
 *    - Total size = 2 + sizeof(Message_t)
 *
 * 5. API Demonstrated:
 *    - ca_message_send() / ca_message_receive(): blocking with timeout
 *    - ca_message_peek_size(): size of next message
 *    - ca_message_count(): number of messages in queue
 *    - ca_message_space_available(): free space
 *    - ca_message_reset(): clears buffer
 *
 * 6. CLI Commands:
 *    - show: message buffer statistics
 *    - clear: clears buffer
 *    - start/stop: enables/disables prints
 *    - help: list commands
 *
 * =============================================================================
 */
