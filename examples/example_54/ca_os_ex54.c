// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 54 -- USB NCM: USB Ethernet (L2 loopback)
 *
 * Registers the ca_usb_ncm service in the ca_usb_dev_mng stack. The device appears
 * as a USB network interface (usb0 on Linux). This example does NOT use the
 * caOS IP stack: it performs L2 loopback of received Ethernet datagrams,
 * swapping source <-> destination MAC so the host receives coherent frames
 * (DA = own MAC, SA = device). Demonstrates wire NTB encode/decode.
 *
 *   ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM);
 *   ca_usb_dev_mng_add(&s_ncm.header);
 *   ca_usb_dev_mng_start();
 *
 * Hardware: H753 (USB_NUM=1), U575 (USB_NUM=0), F407 (USB_NUM=0).
 *
 * Test from PC (X = assigned interface, e.g. usb0 / enxXX):
 *   ip link                          # identify the USB interface
 *   sudo ip addr add 192.168.7.1/24 dev usb0
 *   sudo ip link set usb0 up
 *   # ARP/ping to any IP in the subnet: L2 echo returns the
 *   # frames with SA<->DA swapped. Verify with: sudo tcpdump -i usb0 -e
 *   arping -I usb0 192.168.7.2       # sees the bounced requests return
 *
 * NOTE: without an IP stack on the device, 'ping' does not receive ICMP replies;
 * the success criterion is (1) usb0 enumerated, (2) TX frames bounce back in RX with
 * SA<->DA swapped (tcpdump). Integration with caos/net (ca_net_iface push)
 * is a subsequent step.
 *
 * REQUIREMENTS os_config.h: EN_USB 1 | EN_USB_NCM 1 (CDC/MSC/CUSTOM = 0)
 */

#include "caos.h"


/* ── Configuration for MCU family ────────────────────────────────────────── */

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


#if EN_USB && EN_USB_NCM


/* ── L2 Loopback: swap DA<->SA and resend ────────────────────────────────── */

static u32 s_rx_count;
static u32 s_tx_count;

static void ncm_rx_frame(const u8 *eth, u16 len)
{
    if (len < 14U) return;            /* at least the Ethernet header is needed */
    s_rx_count++;

    /* Local copy: swap the first 12 bytes (DA[0..5] <-> SA[6..11]) */
    static u8 buf[USB_NCM_MAX_FRAME] __attribute__((aligned(4)));
    if (len > sizeof(buf)) return;
    ca_memcpy(buf, eth, len);
    for (u32 i = 0U; i < 6U; i++) {
        u8 t = buf[i]; buf[i] = buf[i + 6U]; buf[i + 6U] = t;
    }

    if (ca_usb_ncm_send_frame(buf, len)) s_tx_count++;
}

/* ── NCM class config (D20: static, persistent lifetime) ────────────────── */

static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x01U },
    .on_rx  = ncm_rx_frame,
};

/* ── Task management ──────────────────────────────────────────────────────── */

static u32 task_usb(void *arg)
{
    (void)arg;
    ca_stdout_put_n("[USB] NCM init...");
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_put_n("[USB] init FAILED");
        return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_put_n("[USB] add NCM FAILED - check EN_USB_NCM");
        return 1U;
    }
    ca_usb_dev_mng_start();
    ca_stdout_put_n("[USB] NCM ready - bring up usb0 on host (loopback L2)");

    bool prev_up = false;
    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev_up) {
            ca_stdout_printf_n("[USB] link %s", up ? "UP" : "DOWN");
            prev_up = up;
        }
        ca_os_sleep(500U);
    }
    return 0U;
}

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

/* ── Diagnostic CLI ──────────────────────────────────────────────────────── */

#if ENABLE_CLI
static bool ncm_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "ncm", 3) != 0) return false;
    CLI_PRINTF_N("ncm: link=%s rx=%lu tx=%lu",
                 ca_usb_ncm_is_up() ? "up" : "down",
                 (u32)s_rx_count, (u32)s_tx_count);
    return true;
}
static ca_cli_node_t s_ncm_cli = { ncm_cli_handler, NULL };
#endif

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
        ca_cli_register(&s_ncm_cli);
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

#else  /* EN_USB && EN_USB_NCM not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m54a, "=== Example 54 ===");
PRIVATE_STRING(m54b, "EN_USB / EN_USB_NCM disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m54a);
    PUT_N(&priv_m54b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB && EN_USB_NCM */
