// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#include "caos.h"

#if EN_STREAM

/*
 * Example of Stream Buffer usage
 *
 * The Stream Buffer is a FIFO circular buffer for byte streams.
 * Unlike the Message Buffer (which preserves message boundaries),
 * the Stream treats data as a continuous stream of bytes.
 *
 * In this example:
 * - A writer task sends data into the stream (simulates data production)
 * - A reader task receives data from the stream (simulates consumption/processing)
 * - A monitor task shows the stream status in real-time
 *
 * Features demonstrated:
 * - ca_stream_init() / ca_stream_init_static()
 * - ca_stream_send() / ca_stream_receive() with timeout
 * - ca_stream_bytes_available() / ca_stream_space_available()
 * - ca_stream_set_trigger() for receiver wake-up
 * - ca_stream_reset() to clear the buffer
 *
 * CLI COMMANDS:
 * - start: enables task prints
 * - stop: disables task prints
 * - show: shows stream statistics
 * - clear: empties the stream buffer
 * - help: list of available commands
 */

#define STREAM_SIZE 64  // Stream buffer size in bytes

/* Static stream buffer (allocation in .bss) */
CA_STREAM_STATIC_DEFINE(data_stream, STREAM_SIZE);

static u32 running;
static u32 bytes_sent;
static u32 bytes_received;
static u32 writer_cycles;
static u32 reader_cycles;
static volatile u8 print_enabled;  // 0 = prints disabled, 1 = enabled

/* =============================================================================
 * TASK: Writer - sends data into the stream
 * ============================================================================= */

static u32 task_writer(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    if (print_enabled) {
        ca_stdout_printf_n("WRITER started: idx=%u p1=%u", idx, p1);
    }

    u8 sequence = 0;
    char tx_buffer[16];
    int tx_len;

    while (running) {
        // Prepare a block of data to send
        tx_len = ca_snprintf(tx_buffer, sizeof(tx_buffer), "DATA_%03d", sequence);

        // Send data into the stream (500ms timeout)
        u32 written = ca_stream_send(&data_stream, (u8*)tx_buffer, tx_len, 500);

        if (written > 0) {
            bytes_sent += written;
            writer_cycles++;
            sequence++;

            if (print_enabled) {
                ca_stdout_printf_n("WRITER: sent %u bytes (seq=%d)", written, sequence - 1);
            }
        } else {
            if (print_enabled) {
                ca_stdout_printf_n("WRITER: timeout (stream full?)");
            }
        }

        // Wait before the next send
        ca_os_sleep(200);
    }

    return 0;
}

/* =============================================================================
 * TASK: Reader - receives data from the stream
 * ============================================================================= */

static u32 task_reader(void *arg) {
    const u32 p1 = (u32)(uintptr_t)arg;
    const u32 idx = ca_os_thid_from_handle(ca_os_self_handle());

    if (print_enabled) {
        ca_stdout_printf_n("READER started: idx=%u p1=%u", idx, p1);
    }

    u8 rx_buffer[32];
    u32 bytes_to_read;

    while (running) {
        // Determine how many bytes to read (max 32 or available ones)
        bytes_to_read = ca_stream_bytes_available(&data_stream);
        if (bytes_to_read > sizeof(rx_buffer)) {
            bytes_to_read = sizeof(rx_buffer);
        }

        if (bytes_to_read > 0) {
            // Receive data from the stream (100ms timeout)
            u32 read = ca_stream_receive(&data_stream, rx_buffer, bytes_to_read, 100);

            if (read > 0) {
                bytes_received += read;
                reader_cycles++;

                // Null-terminate for printing
                rx_buffer[read] = '\0';

                if (print_enabled) {
                    ca_stdout_printf_n("READER: recv %u bytes: '%s'", read, rx_buffer);
                }
            }
        } else {
            // Stream empty, wait
            ca_os_sleep(50);
        }
    }

    return 0;
}

/* =============================================================================
 * TASK: Monitor - shows stream statistics
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
        u32 available = ca_stream_bytes_available(&data_stream);
        u32 free_space = ca_stream_space_available(&data_stream);
        bool is_empty = ca_stream_is_empty(&data_stream);
        bool is_full = ca_stream_is_full(&data_stream);

        if (print_enabled) {
            ca_stdout_printf_n("=== STREAM STATUS ===");
            ca_stdout_printf_n("Bytes available : %u / %u", available, STREAM_SIZE);
            ca_stdout_printf_n("Free space      : %u / %u", free_space, STREAM_SIZE);
            ca_stdout_printf_n("Empty           : %s", is_empty ? "YES" : "NO ");
            ca_stdout_printf_n("Full            : %s", is_full ? "YES" : "NO ");
            ca_stdout_printf_n("Total sent      : %u bytes (%u cycles)", bytes_sent, writer_cycles);
            ca_stdout_printf_n("Total recv      : %u bytes (%u cycles)", bytes_received, reader_cycles);
            ca_stdout_printf_n("=====================");
        }

        // Print every 3 seconds
        ca_os_sleep(3000);
    }

    return 0;
}

/* =============================================================================
 * CLI HOOK - user commands for stream
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
        CLI_PUT_N("--- Stream Buffer Statistics:");
        CLI_PRINTF_N("   Bytes sent    : %u", bytes_sent);
        CLI_PRINTF_N("   Bytes received: %u", bytes_received);
        CLI_PRINTF_N("   Writer cycles : %u", writer_cycles);
        CLI_PRINTF_N("   Reader cycles : %u", reader_cycles);
        CLI_PRINTF_N("   Available     : %u", ca_stream_bytes_available(&data_stream));
        CLI_PRINTF_N("   Free space    : %u", ca_stream_space_available(&data_stream));
        CLI_PRINTF_N("   Print status  : %s", print_enabled ? "ENABLED" : "DISABLED");
        return true;
    }
    if (ca_strncmp(p, "clear", 5) == 0) {
        ca_stream_reset(&data_stream);
        CLI_PUT_N("Stream buffer cleared!");
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };



/* ============================================================================
 * 'twait [sec]': timed wait check on the stream buffer (27/09)
 *
 * A timed waiter parks in the primitive's wait list, SLEEP until its
 * deadline, and the peer wakes it. It used to poll every 10 ms, off the list:
 * woken up to 10 ms late, and served in the order it happened to poll.
 *   latency : a peer task sends one byte after 1..8 ms; the waiter, with a
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
CA_STREAM_STATIC_DEFINE(tw_obj, 16);
static void tw_init(void) {  }
static void tw_hold(void) {  }
static void tw_give(void) { const u8 b = 1U; (void)ca_stream_send(&tw_obj, &b, 1U, CA_NO_WAIT_TIME); }
static bool tw_wait(u32 ms) { u8 b; return ca_stream_receive(&tw_obj, &b, 1U, ms) > 0U; }
static void tw_after(void) {  }
static void tw_drain(void) { ca_stream_reset(&tw_obj); }

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
            CLI_PUT_N("TW stream : peer task not created  FAIL");
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
    CLI_PRINTF_N("TW stream : rounds=%u lat_max=%u/%u lost=%u spur=%u | tmo n=%u early=%u late=%u got=%u over_max=%u/%u%s  %s",
                 rounds, lat_max, lat_lim, lost, spur, n, early, late, got, over_max, slack,
                 stuck ? " STUCK" : "", ok ? "PASS" : "FAIL");
}

static bool tw_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("twait     : [sec] timed wait check on the stream buffer: latency + timeout");
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
    EVAL_STREAM(
        running = 1;
        bytes_sent = 0;
        bytes_received = 0;
        writer_cycles = 0;
        reader_cycles = 0;
        print_enabled = 0;

        /* Set trigger level to 8 bytes for receiver wake-up */
        ca_stream_set_trigger(&data_stream, 8);

        /* Create writer task (medium priority) */
        ca_os_job("writer", CA_TH_LEVEL2, task_writer, NULL,
                  STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);

        /* Create reader task (medium priority) */
        ca_os_job("reader", CA_TH_LEVEL2, task_reader, NULL,
                  STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);

        /* Create monitor task (low priority) */
        ca_os_job("monitor", CA_TH_LEVEL1, task_monitor, NULL,
                  STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

        /* Register CLI hook */
        EVAL_CLI( ca_cli_register(&s_user_cli_node); )
    )
    EVAL_CLI( ca_cli_register(&s_tw_cli_node); )
    /* Start scheduler */
    ca_os_run(true);
}


#else /* EN_STREAM */
/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m14a, "=== Example 14 ===");
PRIVATE_STRING(m14b, "EN_STREAM==0, skipping stream buffer example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m14a);
    PUT_N(&priv_m14b);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_STREAM */

/* =============================================================================
 * IMPORTANT NOTES
 * =============================================================================
 *
 * 1. Stream Buffer vs Message Buffer:
 *    - Stream: continuous byte stream (like a pipe)
 *    - Message: distinct messages with preserved boundaries
 *
 * 2. Trigger Level:
 *    - ca_stream_set_trigger() sets the minimum byte threshold
 *      to wake up the waiting receiver task
 *    - Default: 1 byte
 *
 * 3. Timeout:
 *    - ca_stream_send() / ca_stream_receive() support timeouts
 *    - 0 = non-blocking, 0xFFFFFFFF = infinite wait
 *
 * 4. ISR Variants:
 *    - ca_stream_send_isr() / ca_stream_receive_isr()
 *    - Usable from interrupt handlers (non-blocking)
 *
 * 5. Static vs Heap:
 *    - CA_STREAM_STATIC_DEFINE(): buffer in .bss, zero overhead
 *    - ca_stream_init(): dynamically allocated buffer
 *
 * 6. CLI Commands:
 *    - show: stream statistics
 *    - clear: clears the buffer
 *    - help: list of user commands
 *
 * =============================================================================
 */
