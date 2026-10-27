// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 57 -- USB management stack: DFU runtime (class 0xFE/0x01/0x01)
 *
 * Demonstrates the `ca_usb_dfu` service (D11 usb_stack_workplan.md) over
 * caos/usb/usb_mgmt. DFU runtime exposes a DFU interface without data endpoints
 * (only EP0 control); the host (dfu-util) recognizes it and can send DFU_DETACH
 * to request the transition to DFU mode.
 *
 *   ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)     -> registers the mgmt driver callbacks
 *   ca_usb_dev_mng_add(&dfu.header)  -> registers the DFU class (1 iface, no EP)
 *   ca_usb_dev_mng_start()           -> build descriptors + connect (host enumerates)
 *
 * The management builds the config descriptor and dispatches SETUPs; the
 * DFU service provides iface + functional descriptor and responds to class
 * requests (DETACH/GETSTATUS/GETSTATE/CLRSTATUS/ABORT). Upon DFU_DETACH it calls
 * on_detach(): here we simply signal it (in a real device = set no-init flag
 * + reset to the system bootloader).
 *
 * Supported hardware:
 *   NUCLEO-H753ZI   — OTG_FS CN14   (USB_NUM=1)
 *   NUCLEO-U575ZI-Q — OTG_FS internal (USB_NUM=0)
 *   NUCLEO-F407ZG   — OTG_FS internal (USB_NUM=0)
 *
 * Test from PC (install tools/99-caos-usb.rules once - without it the node is
 * root-owned and read-only, so lsusb sees the device and dfu-util cannot open
 * it: "Cannot open DFU device ca01:0040"):
 *   lsusb                          # VID=0xCA01, PID=0x0040 (DFU)
 *   dfu-util -l                    # shows the DFU interface in runtime
 *   dfu-util -e                    # sends DFU_DETACH -> on_detach on the device
 *
 * REQUIREMENTS os_config.h:
 *   EN_USB 1  |  EN_USB_DFU 1
 */

#include "caos.h"


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


#if EN_USB && EN_USB_DFU

/* ── Configuration for MCU family ─────────────────────────────────────── */



/* ── Shared state ISR ↔ task ──────────────────────────────────────────── */

static volatile u8  s_detach_req;   /* set by dfu_detached() (ISR ctx) */

/* DFU_DETACH received: in a real device here you set a no-init flag and
   perform a reset to the bootloader. In the example we only signal it. */
static void dfu_detached(void)
{
    s_detach_req = 1U;
}

/* ── DFU class config (D20: static, persistent lifetime) ────────────────── */

static const ca_usb_dfu_cfg_t s_dfu = {
    .header            = { CA_USB_TYPE_DFU, &ca_usb_dfu_ops },
    .detach_timeout_ms = 1000U,
    .transfer_size     = 1024U,
    .attributes        = USB_DFU_ATTR_CAN_DNLOAD | USB_DFU_ATTR_WILL_DETACH,
    .iface_str         = "caOS DFU Runtime",
    .on_detach         = dfu_detached,
};

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
        CLI_PRINTF_N("[USB] DFU runtime  detach_req=%u", s_detach_req);
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
        bool ok = ca_usb_dev_mng_add(&s_dfu.header);
        CLI_PRINTF_N("[USB] add dfu: %s", ok ? "ok" : "ko (already/disabled)");
        return true;
    }
    if (ca_strncmp(p, "remove", 6) == 0) {
        bool ok = ca_usb_dev_mng_remove(CA_USB_TYPE_DFU);
        CLI_PRINTF_N("[USB] remove dfu: %s", ok ? "ok" : "ko (not present)");
        return true;
    }

    CLI_PUT_N("usb: status | start | stop | add | remove");
    return true;
}

static ca_cli_node_t s_usb_cli = { usb_cli_handler, NULL };

#endif /* ENABLE_CLI */

/* ── Task management (init + add + start) ─────────────────────────────────── */

static u32 task_usb(void *arg)
{
    (void)arg;

    ca_stdout_put_n("[USB] mng init...");
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_put_n("[USB] init FAILED - check clock/GPIO");
        return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_dfu.header)) {
        ca_stdout_put_n("[USB] add dfu FAILED - check EN_USB_DFU");
        return 1U;
    }
    ca_usb_dev_mng_start();
    ca_stdout_put_n("[USB] D+ pull-up on - plug cable (DFU runtime, dfu-util -l)");

    /* Reports the host's DFU_DETACH request to the console. In a real
       device: ca_flash/no-init flag + reset to bootloader. */
    u8 det_prev = 0U;
    while (1) {
        u8 det = s_detach_req;
        if (det != det_prev) {
            ca_stdout_put_n("[USB] DFU_DETACH received - the app would switch to DFU mode "
                            "(here: notification only)");
            det_prev = det;
        }
        ca_os_sleep(100U);
    }
    return 0U;
}

/* ── Task heartbeat ───────────────────────────────────────────────────────── */

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
    ca_os_job("usb", CA_TH_LEVEL6, task_usb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("hb", CA_TH_LEVEL4, task_hb, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_run(true);
}

#else  /* EN_USB && EN_USB_DFU not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m57a, "=== Example 57 ===");
PRIVATE_STRING(m57b, "EN_USB / EN_USB_DFU disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m57a);
    PUT_N(&priv_m57b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB && EN_USB_DFU */
