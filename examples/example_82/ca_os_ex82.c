// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 82 -- caOS TFTP client + DHCP, over USB-NCM
 *
 * NCM variant of example_49 (same service on ETH). The application logic is
 * copied unchanged: what differs is only the interface handed to
 * ca_net_init(), thanks to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client — obtains IP automatically
 *   2. ARP service — cache + blocking resolve
 *   3. ICMP echo reply (ping)
 *   4. CLI — interactive commands
 *   5. TFTP client — GET and PUT via CLI:
 *        tftp get <ip> <filename>
 *        tftp put <ip> <filename> [size]   (buffer pattern in RAM, def 1024B)
 *        tftp info
 *
 * The TFTP client speaks first, but only when the user types the command: by
 * then the link is necessarily up, so it needs no gate of its own -- unlike
 * DHCP, which starts on its own as soon as the task runs. ca_tftp_service_init()
 * only registers the CLI node (tftp_service.c:670), it puts nothing on the wire,
 * so it stays in ca_os_configure() as on ETH. The local IP is read from
 * s_net_proto_local_ip when the command runs, so the lease is picked up with no
 * extra wiring.
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side (built-in DHCP server; --put to accept uploads from the board):
 *   sudo python3 examples/example_82/test_tftp_ncm.py --put
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_TFTP_SERVICE 1 | EN_CLI_TFTP 1 | ENABLE_CLI 1
 */

#include "caos.h"
#include "net_proto.h"

/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
#define USB_NUM      0U

#elif MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U

#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS — single instance H563 */

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#endif


#if EN_NET && EN_USB && EN_USB_NCM && EN_NET_DHCP && \
    EN_DHCP_SERVICE && EN_TFTP_SERVICE

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x82U } };

static volatile ca_ipv4_t s_ip        = { .addr = 0U };
static volatile bool   s_dhcp_done = false;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xC2U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * DHCP port 68 handler
 * ============================================================================ */
static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b   = ca_net_buf_ptr(buf_idx);
    u8  *frame        = b->data + b->data_off;
    u16  l4_off       = b->meta.rx.l4_off;
    const ca_udp_frame_t *udp = (const ca_udp_frame_t *)(frame + l4_off);
    u16 udp_total = (u16)(b->data_len - l4_off);
    ca_dhcp_service_rx_handler(udp, udp_total);
    ca_net_buf_free(buf_idx);
}

/* ============================================================================
 * DHCP callbacks
 * ============================================================================ */
static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    s_ip        = ip;
    s_dhcp_done = true;

    ca_net_proto_set_local_ip(ip);
#if EN_ARP_SERVICE
    ca_arp_service_update_local_ip(ip);
#endif

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  mask=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3],
        mask.v[0], mask.v[1], mask.v[2], mask.v[3]);
    ca_stdout_printf_n("            GW=%u.%u.%u.%u  DNS=%u.%u.%u.%u",
        gw.v[0], gw.v[1], gw.v[2], gw.v[3],
        dns.v[0], dns.v[1], dns.v[2], dns.v[3]);
    (void)dns;
#if EN_ARP_SERVICE
    ca_arp_send_garp();
#endif
    ca_stdout_put_n("ready: tftp get <ip_server> <file>  |  tftp put <ip_server> <file> [size]");
}

static void on_dhcp_expire(void)
{
    s_dhcp_done = false;
    s_ip.addr   = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x82U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX82: DHCP + TFTP client over USB-NCM ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex49) */
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_printf_n("[USB] init FAILED"); return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_printf_n("[USB] add NCM FAILED - check EN_USB_NCM"); return 1U;
    }
    ca_usb_dev_mng_start();

    /* Attach NCM to the net core as push iface */
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_printf_n("[NET] ca_net_init FAILED"); return 1U;
    }

    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    /* The DORA must not start before the host has brought the interface up:
     * the NCM link goes up only when the host selects alt setting 1, and until
     * then ca_usb_ncm_send_frame() drops every frame and returns false -- a
     * DISCOVER sent too early is LOST, not delayed. But the wait CANNOT sit
     * before the loop: enumeration needs this task to be already servicing
     * frames, so blocking here would hold off the very event being waited for.
     *
     * TFTP needs no gate: it only speaks when the user types the command, and
     * by then the link has long been up. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            EVAL_DHCP_SVC(ca_dhcp_service_net_ready();)
            ca_stdout_put_n("DHCP: starting DORA...");
        }

        /* Timed, not ca_sem_wait(): before the link is up no frame ever
         * arrives, so a plain wait would park here and the gate above would
         * never be re-evaluated. */
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) {
            ca_net_process();
        }
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex49
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            if (up && s_dhcp_done) {
                ca_stdout_put_n("link back UP: requesting lease renew");
                ca_dhcp_service_renew();
            }
            prev = up;
        }
        ca_led_toggle(1U);
        ca_os_sleep(500);
    }
    return 0U;
}

/* ── Diagnostic CLI ──────────────────────────────────────────────────────── */
#if ENABLE_CLI
static bool dbg_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "usbdbg", 6) != 0) return false;
    EVAL_USB_DBG(ca_usb_dbg_print(USB_NUM);)
    return true;
}
static ca_cli_node_t s_dbg_cli = { dbg_cli_handler, NULL };
#endif

/* ============================================================================
 * caOS hooks
 * ============================================================================ */
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
        ca_cli_register(&s_dbg_cli);
    )

    ca_led_init();
    ca_led_off(1U);

    /* ca_net_init() is NOT called here (unlike ex49): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */

    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })
    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)

    /* Registers the "tftp" CLI node only — no network traffic here, so it can
     * stay in configure() exactly as on ETH. */
    EVAL_TFTP_SVC(ca_tftp_service_init(s_mac);)
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)

    ca_os_job("net",     CA_TH_LEVEL4, task_net,     NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon", CA_TH_LEVEL3, task_ncm_mon, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m82a, "=== Example 82: TFTP client + DHCP over USB-NCM ===");
PRIVATE_STRING(m82b, "EN_NET / EN_USB / EN_USB_NCM / EN_DHCP_SERVICE / EN_TFTP_SERVICE == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m82a);
    PUT_N(&priv_m82b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_TFTP_SERVICE && ... */
