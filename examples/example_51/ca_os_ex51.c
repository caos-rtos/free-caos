// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 51 -- USB management stack: custom class (vendor 0xFF + 1 EP bulk)
 *
 * Demonstrates the high-level USB stack (caos/usb/usb_mgmt) using the
 * `ca_usb_custom` service as a reference implementation of ca_usb_class_ops_t:
 *
 *   ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)      -> registers mgmt driver callbacks
 *   ca_usb_dev_mng_add(&custom.header) -> registers the custom class (vendor + 1 EP)
 *   ca_usb_dev_mng_start()            -> build descriptors + connect (host enumerates)
 *
 * The management builds composite device/config descriptors and dispatches
 * standard SETUPs; the custom service provides only the interface descriptor,
 * opens the EP on SET_CONFIGURATION and performs fan-out of on_rx/on_tx_done.
 * The loopback (OUT -> IN) lives here, in task context.
 *
 * Difference from example_50: there, the descriptor and the entire EP0 were in the app;
 * here, they are in the mgmt. The app provides only the class cfg + the loopback.
 *
 * Supported hardware:
 *   NUCLEO-H753ZI   — OTG_FS CN14   (USB_NUM=1)
 *   NUCLEO-U575ZI-Q — internal OTG_FS (USB_NUM=0)
 *   NUCLEO-F407ZG   — internal OTG_FS (USB_NUM=0)
 *
 * Test from PC:
 *   lsusb                          # VID=0xCA01, PID=0x0001 (CUSTOM ep_data=1)
 *   sudo python3 test_usb_custom.py # loopback EP1
 *
 * REQUIREMENTS os_config.h:
 *   EN_USB 1  |  EN_USB_CUSTOM 1
 */

#include "caos.h"

/* ── Configuration for MCU family ───────────────────────────────────────── */

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



#if EN_USB && EN_USB_CUSTOM

/* ── Configuration for MCU family ───────────────────────────────────────── */



#define CUSTOM_EP_DATA  1U   /* EP1 IN + EP1 OUT bulk */

/* ── Shared state ISR ↔ task ────────────────────────────────────────────── */

/* Handle, not thid: a thid is a list position and moves when a task below it ends. */
static ca_task_handle_t s_loop_task = CA_TASK_HANDLE_NONE;
static volatile u32  s_rx_cnt;
static volatile u32  s_tx_cnt;

#define NOTIFY_BULK_RX  (1U << 0)

/* a loopback buffer (ep_write copies before returning → reusable) */
static u8 s_loop_buf[EP_BULK_MPS] __attribute__((aligned(4)));

/* ── Custom service callbacks (called by ISR via mng_on_rx fan-out) ──────── */

static void custom_rx(u8 ep_addr, u16 len)
{
    (void)ep_addr; (void)len;
    s_rx_cnt++;
    ca_notify_send_handle_isr(s_loop_task, NOTIFY_BULK_RX, CA_NOTIFY_ACTION_SET_BITS);
}

static void custom_tx_done(u8 ep_addr)
{
    (void)ep_addr;
    /* TX IN handled by the per-EP driver queue → no action */
}

/* Mount/unmount: update a volatile state reported by the task (ISR-safe). */
static volatile u8 s_mounted;

static void custom_mounted(void)   { s_mounted = 1U; }
static void custom_unmounted(void) { s_mounted = 0U; }

/* ── Custom class config (D20: must be static, persistent lifetime) ──────── */

static const ca_usb_custom_cfg_t s_custom = {
    .header       = { CA_USB_TYPE_CUSTOM, &ca_usb_custom_ops },
    .ep_data      = CUSTOM_EP_DATA,
    .mps          = EP_BULK_MPS,
    .on_rx        = custom_rx,
    .on_tx_done   = custom_tx_done,
    .on_mounted   = custom_mounted,
    .on_unmounted = custom_unmounted,
};

/* ── Bulk loopback (task context) ────────────────────────────────────────── */

static void loop_drain(void)
{
    u8  ep_out = CUSTOM_EP_DATA;
    u8  ep_in  = (u8)(CUSTOM_EP_DATA | 0x80U);
    u16 len;
    while ((len = ca_usb_ep_read(USB_NUM, ep_out, s_loop_buf, EP_BULK_MPS)) > 0U) {
        while (!ca_usb_ep_write(USB_NUM, ep_in, s_loop_buf, len))
            ca_os_yield();
        s_tx_cnt++;
    }
}

/* ── CLI ──────────────────────────────────────────────────────────────────── */

#if ENABLE_CLI

static bool usb_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("usb: status | start | stop | add | remove");
        return false;
    }
    if (ca_strncmp(p, "usb", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "status", 6) == 0) {
        CLI_PRINTF_N("[USB] custom ep=%u mps=%u  %s  rx=%lu tx=%lu",
                     CUSTOM_EP_DATA, EP_BULK_MPS,
                     s_mounted ? "mounted" : "unmounted", s_rx_cnt, s_tx_cnt);
        return true;
    }
    if (ca_strncmp(p, "start", 5) == 0) {
        ca_usb_dev_mng_start();
        CLI_PUT_N("[USB] start - host enumerate");
        return true;
    }
    if (ca_strncmp(p, "stop", 4) == 0) {
        ca_usb_dev_mng_stop();
        CLI_PUT_N("[USB] stop - host disconnect");
        return true;
    }
    if (ca_strncmp(p, "add", 3) == 0) {
        bool ok = ca_usb_dev_mng_add(&s_custom.header);
        CLI_PRINTF_N("[USB] add custom: %s", ok ? "ok" : "ko (already/disabled)");
        return true;
    }
    if (ca_strncmp(p, "remove", 6) == 0) {
        bool ok = ca_usb_dev_mng_remove(CA_USB_TYPE_CUSTOM);
        CLI_PRINTF_N("[USB] remove custom: %s", ok ? "ok" : "ko (not present)");
        return true;
    }

    CLI_PUT_N("usb: status | start | stop | add | remove");
    return true;
}

static ca_cli_node_t s_usb_cli = { usb_cli_handler, NULL };

#endif /* ENABLE_CLI */

/* ── Loopback task ────────────────────────────────────────────────────────── */

static u32 task_loop(void *arg)
{
    (void)arg;
    while (1) {
        (void)ca_notify_wait(true, 2000U);
        loop_drain();
    }
    return 0U;
}

/* ── Management task (init + add + start) ─────────────────────────────────── */

static u32 task_usb(void *arg)
{
    (void)arg;

    ca_stdout_put_n("[USB] mng init...");
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_put_n("[USB] init FAILED - check clock/GPIO");
        return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_custom.header)) {
        ca_stdout_put_n("[USB] add custom FAILED - check EN_USB_CUSTOM");
        return 1U;
    }
    ca_usb_dev_mng_start();
    ca_stdout_put_n("[USB] D+ pull-up on - plug cable (custom vendor loopback EP1)");

    /* Reports enumeration state changes (mount/unmount) to the console. */
    u8 mnt_prev = 0U;
    while (1) {
        u8 mnt = s_mounted;
        if (mnt != mnt_prev) {
            ca_stdout_put_n(mnt ? "[USB] host enumerated device (mounted)"
                                : "[USB] host disconnected (unmounted)");
            mnt_prev = mnt;
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
}

void ca_os_main(void)
{
    /* Loopback task (woken by custom_rx). Created before task_usb so
       s_loop_task is valid when the first on_rx arrive. */
    s_loop_task = ca_os_job("lp1", CA_TH_LEVEL5, task_loop, NULL,
                               STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("usb", CA_TH_LEVEL6, task_usb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("hb", CA_TH_LEVEL4, task_hb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_run(true);
}

#else  /* EN_USB && EN_USB_CUSTOM not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m51a, "=== Example 51 ===");
PRIVATE_STRING(m51b, "EN_USB / EN_USB_CUSTOM disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m51a);
    PUT_N(&priv_m51b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB && EN_USB_CUSTOM */
