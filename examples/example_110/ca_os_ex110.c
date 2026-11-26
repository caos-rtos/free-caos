// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 110 -- USB CDC-ACM at HIGH SPEED: virtual serial port (/dev/ttyACMx)
 *
 * The high-speed twin of example 52: same CDC service, same echo, driven
 * by the HS driver (EN_USB_HS=1, EN_USB_FS=0):
 *   ca_usb_dev_mng_init(CA_USB_SPEED_HS, USB_NUM);
 *   ca_usb_dev_mng_add(&s_cdc.header);
 *   ca_usb_dev_mng_start();
 *
 * The bulk MPS is left at 0 in the CDC cfg: the class takes it from the
 * negotiated speed (512 at HS, 64 if the host keeps the link at FS), and the
 * stack answers DEVICE_QUALIFIER.
 *
 * Supported hardware: H773 (NUCLEO-H7S3L8, OTG_HS + integrated HS PHY, USB_NUM=0).
 *
 * Test from PC:
 *   lsusb -t                          # the device must sit on a 480M port
 *   lsusb -v -d ca01:                 # IAD + CDC, bulk wMaxPacketSize 512
 *   examples/example_52/test_usb_cdc.py
 *
 * REQUIREMENTS os_config.h: EN_USB 1 | EN_USB_CDC 1 | EN_USB_HS 1 | EN_USB_FS 0
 */

#include "caos.h"


/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 773 && EN_USB_HS
#define USB_NUM      0U          /* OTG_HS, integrated HS PHY (Type-C CN2) */

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#endif



#if EN_USB && EN_USB_CDC


#define CDC_MPS  0U      /* 0 = per negotiated speed: 512 HS, 64 FS */

/* ── RX: PULL mode (the task reads, the driver queue NAKs when full) ─────── */

/* Handle, not thid: a thid is a list position and moves when a task below it ends. */
static ca_task_handle_t s_echo_task = CA_TASK_HANDLE_NONE;

#define NOTIFY_RX  (1U << 0)

/* ── CDC Callback (ISR context, via mng fan-out) ─────────────────────────── */

/* PULL mode: the ISR only wakes the echo task, which drains with
 * ca_usb_cdc_read(). Unread packets stay in the driver queue and the endpoint
 * NAKs the host: a flood is slowed down, never dropped (the old ISR ring was). */
static void cdc_rx_ready(void)
{
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
    .on_rx_ready = cdc_rx_ready,
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

/* usb — the line arrives without the command word */
static bool usb_cmd(c_str *args)
{
    const char *p = (*args)->data;
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
static const ca_cli_cmd_t s_usb_cmds[] = {
    { "usb", "usb       : status | dbg reset | dbg | eps | halt | resume | help", { usb_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_usb_table = {
    "usb", s_usb_cmds, (u16)(sizeof(s_usb_cmds) / sizeof(s_usb_cmds[0])), NULL
};
#endif

/* ── Echo task (serial loopback) ─────────────────────────────────────────── */



static u32 task_echo(void *arg)
{
    (void)arg;
    static string msg;
    while (1) {
        (void)ca_notify_wait(true, 2000U);
        /* Drain everything that is pending: one notify can stand for several
         * packets, and a packet can be bigger than a string (512 B at HS). */
        u16 n;
        while ((n = ca_usb_cdc_read((u8 *)msg.data, (u16)sizeof(msg.data))) > 0U) {
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
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_HS, USB_NUM)) {
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
        ca_cli_register_table(&s_usb_table);
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
PRIVATE_STRING(m110a, "=== Example 110 ===");
PRIVATE_STRING(m110b, "USB CDC high speed: NUCLEO-H7S3L8 only (EN_USB_HS, EN_USB_CDC)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m110a);
    PUT_N(&priv_m110b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB && EN_USB_CDC */
