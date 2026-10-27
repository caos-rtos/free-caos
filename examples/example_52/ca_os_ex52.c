// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 52 -- USB CDC-ACM: virtual serial port (/dev/ttyACMx)
 *
 * Register the ca_usb_cdc service in the ca_usb_dev_mng stack:
 *   ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM);
 *   ca_usb_dev_mng_add(&s_cdc.header);
 *   ca_usb_dev_mng_start();
 *
 * The device enumerates as CDC ACM (IAD + comm-IF + data-IF). The PC sees
 * /dev/ttyACM0. The example performs an echo (serial loopback): every byte received from
 * the host is sent back, and a welcome prompt is sent upon opening (DTR).
 *
 * Supported hardware: H753 (USB_NUM=1), U575 (USB_NUM=0), F407 (USB_NUM=0).
 *
 * Test from PC:
 *   lsusb -v -d ca01:                 # IAD + CDC Communications + CDC Data
 *   sudo screen /dev/ttyACM0 115200   # type → it is echoed
 *   (or: examples/example_52/test_usb_cdc.py)
 *
 * REQUIREMENTS os_config.h: EN_USB 1 | EN_USB_CDC 1
 */

#include "caos.h"


/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
#define USB_NUM      0U
#define EP_BULK_MPS  64U

#elif MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */
#define EP_BULK_MPS  64U   /* 64 for OTG_FS, 512 for OTG_HS ULPI */

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U
#define EP_BULK_MPS  64U

#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS — single instance H563 */
#define EP_BULK_MPS  64U

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#endif



#if EN_USB && EN_USB_CDC


#define CDC_MPS  64U

/* ── RX Ring (ISR → task) ─────────────────────────────────────────────────── */

#define RX_RING_SIZE  512U
static volatile u8  s_rx_ring[RX_RING_SIZE];
static volatile u16 s_rx_head;   /* written by ISR (on_rx) */
static volatile u16 s_rx_tail;   /* read by task */
/* Handle, not thid: a thid is a list position and moves when a task below it ends. */
static ca_task_handle_t s_echo_task = CA_TASK_HANDLE_NONE;

#define NOTIFY_RX  (1U << 0)

/* ── CDC Callback (ISR context, via mng fan-out) ─────────────────────────── */

static void cdc_rx(const u8 *buf, u16 len)
{
    for (u16 i = 0U; i < len; i++) {
        u16 next = (u16)((s_rx_head + 1U) % RX_RING_SIZE);
        if (next == s_rx_tail) break;          /* ring full: drop */
        s_rx_ring[s_rx_head] = buf[i];
        s_rx_head = next;
    }
    ca_notify_send_handle_isr(s_echo_task, NOTIFY_RX, CA_NOTIFY_ACTION_SET_BITS);
}

static volatile u8  s_dtr;         /* last known DTR state */
static volatile u8  s_last_ctrl;   /* diag: last wValue control line state */
static volatile u32 s_line_evt;    /* diag: number of on_line calls */
static volatile u8  s_halted;      /* CLI 'usb halt' → soft-disconnect active */

PRIVATE_STRING(hello, "caOS CDC ready -- echo on\r\n");

static void cdc_line(const ca_usb_cdc_line_t *line, u8 control)
{
    (void)line;
    s_last_ctrl = control;
    s_line_evt++;
    s_dtr = (u8)(control & 0x01U);             /* bit0 = DTR (terminal opened) */
    if (s_dtr) {                               /* host has opened the terminal */
        ca_usb_cdc_write((p_str)&priv_hello);
    }
}

/* ── CDC class config (D20: static const) ────────────────────────────────── */

static const ca_usb_cdc_cfg_t s_cdc = {
    .header  = { CA_USB_TYPE_CDC, &ca_usb_cdc_ops },
    .mps     = CDC_MPS,
    .on_rx   = cdc_rx,
    .on_line = cdc_line,
};

/* ── Diagnostic CLI ───────────────────────────────────────────────────── */

#if ENABLE_CLI

/* Driver event counters exist with EN_USB_DBG=1. */
#define USB_DBG_AVAIL  EN_USB_DBG

static void usb_cli_help(void)
{
    CLI_PUT_N("usb commands:");
    CLI_PUT_N("  usb status     - DTR, line coding events, connection state");
    CLI_PUT_N("  usb dbg        - ISR counters (session rst/enum/susp/wkup + per-EP)");
    CLI_PUT_N("  usb eps        - per-EP state (CHEP/ring/counters) [EN_USB_DBG]");
    CLI_PUT_N("  usb dbg reset  - clear the counters");
    CLI_PUT_N("  usb halt       - soft-disconnect (D+ down, host sees an unplug)");
    CLI_PUT_N("  usb resume     - reconnect (host re-enumerates)");
    CLI_PUT_N("  usb help       - this help");
}

static bool usb_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "usb", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "status", 6) == 0) {
        CLI_PRINTF_N("[USB] CDC dtr=%u  line_evt=%lu  last_ctrl=0x%02x",
                     s_dtr, s_line_evt, s_last_ctrl);
        CLI_PRINTF_N("[USB] connected=%s", ca_usb_cdc_connected() ? "yes" : "no");
        return true;
    }
    if (ca_strncmp(p, "dbg reset", 9) == 0) {
#if USB_DBG_AVAIL
        ca_usb_dbg_reset();
        CLI_PUT_N("[USB] dbg counters reset");
#else
        CLI_PUT_N("[USB] dbg not available (EN_USB_DBG=0 or MCU not supported)");
#endif
        return true;
    }
    if (ca_strncmp(p, "dbg", 3) == 0) {
#if USB_DBG_AVAIL
        ca_usb_dbg_print(USB_NUM);
#else
        CLI_PUT_N("[USB] dbg not available (EN_USB_DBG=0)");
#endif
        return true;
    }
    if (ca_strncmp(p, "eps", 3) == 0) {
#if USB_DBG_AVAIL
        /* Per-EP status: CHEP register (STAT_RX/TX, DTOG, VTRX/VTTX) + ring.
           STAT: 0=DIS 1=STALL 2=NAK 3=VLD. For CDC: comm IF = EP interrupt,
           data IF = EP bulk IN/OUT. */
        static const char *const stat_nm[4] = { "DIS", "STL", "NAK", "VLD" };
        ca_usb_dbg_t d;
        ca_usb_dbg_stat(&d, USB_NUM);
        for (u8 e = 1U; e < USB_EP_MAX; e++) {
            u32 c   = d.chep[e];
            u8  srx = (u8)((c >> 12) & 0x3U);
            u8  stx = (u8)((c >>  4) & 0x3U);
            u8  drx = (u8)((c >> 14) & 0x1U);
            u8  dtx = (u8)((c >>  6) & 0x1U);
            CLI_PRINTF_N("EP%u CHEP=%08lx RX=%s TX=%s Drx=%u Dtx=%u | rxq h%u/t%u txq h%u/t%u act=%u",
                         e, c, stat_nm[srx], stat_nm[stx], drx, dtx,
                         d.rxq_h[e], d.rxq_t[e], d.txq_h[e], d.txq_t[e], d.tx_act[e]);
            CLI_PRINTF_N("    oxfrc=%lu ixfrc=%lu rd=%lu wr=%lu wrfull=%lu rxdrop=%lu",
                         d.oep_xfrc[e], d.iep_xfrc[e], d.ep_read[e], d.ep_write[e],
                         d.wr_full[e], d.rx_drop[e]);
        }
#else
        CLI_PUT_N("[USB] eps not available (EN_USB_DBG=0)");
#endif
        return true;
    }
    if (ca_strncmp(p, "halt", 4) == 0) {
        if (s_halted) {
            CLI_PUT_N("[USB] already halted");
        } else {
            ca_usb_disconnect(USB_NUM);   /* SDIS=1: D+ down → host sees disconnection */
            s_halted = 1U;
            CLI_PUT_N("[USB] halted (soft disconnect)");
        }
        return true;
    }
    if (ca_strncmp(p, "resume", 6) == 0) {
        if (!s_halted) {
            CLI_PUT_N("[USB] not halted");
        } else {
            ca_usb_connect(USB_NUM);      /* SDIS=0: D+ pull-up → host re-enumerates */
            s_halted = 0U;
            CLI_PUT_N("[USB] resume (host re-enumerate)");
        }
        return true;
    }
    if (ca_strncmp(p, "help", 4) == 0) {
        usb_cli_help();
        return true;
    }
    usb_cli_help();
    return true;
}
static ca_cli_node_t s_usb_cli = { usb_cli_handler, NULL };
#endif

/* ── Echo task (serial loopback) ─────────────────────────────────────────── */



static u32 task_echo(void *arg)
{
    (void)arg;
    static string msg;
    u16 n = 0U;
    while (1) {
        (void)ca_notify_wait(true, 5000U);
        n = 0U;
        while (s_rx_tail != s_rx_head && n < sizeof(msg.data)) {
            msg.data[n++] = (char)s_rx_ring[s_rx_tail];
            s_rx_tail = (u16)((s_rx_tail + 1U) % RX_RING_SIZE);
        }
        if (n > 0U) {
            msg.size = n;
            while (!ca_usb_cdc_write(&msg))
                ca_os_yield();
        }
    }
    return 0U;
}

/* ── Management task ──────────────────────────────────────────────────────── */

static u32 task_usb(void *arg)
{
    (void)arg;
    ca_stdout_put_n("[USB] CDC init...");
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_put_n("[USB] init FAILED");
        return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_cdc.header)) {
        ca_stdout_put_n("[USB] add CDC FAILED - check EN_USB_CDC");
        return 1U;
    }
    ca_usb_dev_mng_start();   /* enable D+ pull-up: the host enumerates if the cable is present */
    ca_stdout_put_n("[USB] D+ pull-up on - waiting host (plug cable)");

    /* Report CDC terminal state changes on the debug console
       (DTR asserted by the host when it opens the virtual serial port). */
    u8 dtr_prev = 0U;
    while (1) {
        u8 dtr = s_dtr;
        if (dtr != dtr_prev) {
            ca_stdout_put_n(dtr ? "[USB] host opened CDC terminal (DTR on)"
                                : "[USB] host closed CDC terminal (DTR off)");
            dtr_prev = dtr;
        }
        ca_os_sleep(100U);
    }
    return 0U;
}

/* ── Heartbeat task ───────────────────────────────────────────────────────── */

static u32 task_hb(void *arg)
{
    (void)arg;
    ca_led_init();
    while (1) {
        ca_led_toggle(1U);
        ca_os_sleep(500U);
    }
    return 0U;
}

/* ── Entry point ──────────────────────────────────────────────────────────── */

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
        ca_cli_register(&s_usb_cli);
    )
    EVAL_PWR( ca_pwr_init(); )
    EVAL_RTC( ca_rtc_init(CA_RTC_CLK_AUTO); )   /* wake source for sleep/stop/standby */
}

void ca_os_main(void)
{
    s_echo_task = ca_os_job("echo", CA_TH_LEVEL5, task_echo, NULL,
                               STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("usb", CA_TH_LEVEL6, task_usb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("hb", CA_TH_LEVEL4, task_hb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* EN_USB && EN_USB_CDC not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m52a, "=== Example 52 ===");
PRIVATE_STRING(m52b, "EN_USB / EN_USB_CDC disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m52a);
    PUT_N(&priv_m52b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB && EN_USB_CDC */
