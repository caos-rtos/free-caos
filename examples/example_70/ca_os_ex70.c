// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 70 -- caOS DHCP client + ARP + ICMP ping + UDP server, over USB-NCM
 *
 * NCM variant of example_35 (same services on ETH). The application logic is
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically (DORA + T1/T2 renew)
 *   2. ARP service + ICMP echo reply on the leased address
 *   3. UDP server on port 7961, up to MAX_CLIENTS clients
 *   4. GARP sent after every DHCP bound/renew
 *   5. CLI with commands dhcp, udp, net
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * IMPORTANT -- there is no DHCP server on a point-to-point USB link:
 * unlike ETH, where the LAN provides one, here the PC must run it on usb0:
 *
 *   examples/example_70/test_dhcp_ncm_pc.sh          # sets up dnsmasq + tests
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 | EN_UDP_SERVER 1
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


#if EN_NET && EN_USB && EN_USB_NCM && EN_NET_DHCP && EN_DHCP_SERVICE && EN_UDP_SERVER

/* ============================================================================
 * Constants
 * ============================================================================ */
#define UDP_SRV_PORT       7961U
#define MAX_CLIENTS        2U          /* as in ex35 */
#define CLIENT_TIMEOUT_MS  15000U

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x70U } };

/* Dynamic IP — written by on_dhcp_bound, read by task_net and CLI */
static volatile ca_ipv4_t s_ip   = { .addr = 0U };
static volatile ca_ipv4_t s_mask = { .addr = 0U };
static volatile ca_ipv4_t s_gw   = { .addr = 0U };
static volatile ca_ipv4_t s_dns  = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xB0U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * DHCP UDP handler (port 68) — called by the net dispatcher
 * ============================================================================ */
static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b = ca_net_buf_ptr(buf_idx);
    u8 *frame = b->data + b->data_off;
    u16 l4_off = b->meta.rx.l4_off;
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
    s_mask      = mask;
    s_gw        = gw;
    s_dns       = dns;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    EVAL_UDP_SRV(ca_udp_server_set_local_ip(ip);)

#if EN_ARP_SERVICE
    ca_arp_service_update_local_ip(ip);
#endif

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  mask=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3],
        mask.v[0], mask.v[1], mask.v[2], mask.v[3]);
    ca_stdout_printf_n("            GW=%u.%u.%u.%u  DNS=%u.%u.%u.%u",
        gw.v[0], gw.v[1], gw.v[2], gw.v[3],
        dns.v[0], dns.v[1], dns.v[2], dns.v[3]);

    ca_arp_send_garp();
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED - renewing...");
}

/* ============================================================================
 * UDP server callbacks
 * ============================================================================ */
static void on_connect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] client %u: %u.%u.%u.%u:%u connected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_disconnect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] client %u: %u.%u.%u.%u:%u disconnected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_rx(u8 cid, ca_ipv4_t ip, u16 port, const u8 *pld, u16 len)
{
    char txt[64U];
    u16 tlen = len < (u16)(sizeof(txt) - 1U) ? len : (u16)(sizeof(txt) - 1U);
    for (u16 i = 0U; i < tlen; i++) {
        u8 c = pld[i];
        txt[i] = (c >= 0x20U && c < 0x7FU) ? (char)c : '.';
    }
    txt[tlen] = '\0';
    ca_stdout_printf_n("UDP[7961] cid=%u rx %uB ('%s') from %u.%u.%u.%u:%u",
        (u32)cid, (u32)len, txt,
        ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);

    u8 reply[256U];
    static const u8 pfx[] = "ECHO: ";
    const u16 pfxlen = (u16)(sizeof(pfx) - 1U);
    u16 rlen = pfxlen + len;
    if (rlen > (u16)sizeof(reply)) rlen = (u16)sizeof(reply);
    for (u16 i = 0U; i < pfxlen; i++) reply[i] = pfx[i];
    u16 dlen = rlen - pfxlen;
    for (u16 i = 0U; i < dlen; i++) reply[pfxlen + i] = pld[i];
    ca_udp_server_send(cid, reply, rlen);
}

/* cfg completed after DHCP bound (dynamic local_ip) */
static ca_udp_server_cfg_t srv_cfg = {
    .port              = UDP_SRV_PORT,
    .max_clients       = MAX_CLIENTS,
    .client_timeout_ms = CLIENT_TIMEOUT_MS,
    .local_mac         = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x70U } },
    .local_ip          = { .addr = 0U },   /* updated after DHCP */
    .on_rx             = on_rx,
    .on_connect        = on_connect,
    .on_disconnect     = on_disconnect,
};

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },   /* 0.0.0.0 — updated after DHCP */
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x70U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX70: DHCP + ARP + ping + UDP server over USB-NCM ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_put_n("IP   --- (waiting for DHCP)");

    /* USB NCM bring-up (replaces eth_gpio_init() of ex35) */
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

    /* Init stack with IP=0, MAC known */
    ca_net_proto_init(&net_cfg);

    /* Register port 68 to receive OFFER/ACK */
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    /* Register port 7961 for UDP server */
    EVAL_UDP_SRV(ca_udp_server_bind();)

    /* The DORA must not start before the host has brought the interface up.
     *
     * This gate has no equivalent on ETH and is NOT a formality: the NCM link
     * goes up only when the host selects alt setting 1 (usb_ncm.c), and until
     * then ca_usb_ncm_send_frame() drops every frame on the floor and returns
     * false. A DISCOVER sent too early is not delayed, it is LOST -- and the
     * DHCP client would spend its whole retry budget (DHCP_DISCOVER_RETRY_N x
     * DHCP_OFFER_TIMEOUT_MS) talking into a closed pipe.
     *
     * But the wait CANNOT sit before the loop below: enumeration needs this
     * task to be already servicing frames, so blocking here would hold off the
     * very event being waited for (that mistake stalls enumeration outright --
     * the host reports "device not accepting address"). The gate therefore
     * lives INSIDE the loop, and the DHCP task is released the first time the
     * link is seen up. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            /* Unblocks the DHCP task (was waiting for this gate).
             * From this moment on, port 68 is registered and DHCP frames are
             * delivered to dhcp_udp_handler.
             *
             * NB ca_dhcp_service_bind() is deliberately NOT called: it blocks,
             * and would stop ca_net_process() from running -> the OFFER would
             * never be processed -> deadlock. The IP arrives through the
             * on_dhcp_bound callback from the DHCP task (LEVEL1). */
            EVAL_DHCP_SVC(ca_dhcp_service_net_ready();)
            ca_stdout_put_n("DHCP: starting DORA...");
        }

        /* Timed, not ca_sem_wait(): before the link is up no frame ever
         * arrives, so a plain wait would park here and the gate above would
         * never be re-evaluated. The timeout makes this loop poll the link
         * while still sleeping between frames. */
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) {
            ca_net_process();
        }
    }
    return 0U;
}

/* No "eth stats"/"eth pool" here: that CLI lives inside #if EN_ETH in
 * caos/drivers/src/eth.c, so it is not built on NCM (EN_CLI_ETH is off).
 * Pool and net counters stay reachable through "net" (EN_CLI_NET). */

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex35
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            /* Coming back up with a lease already taken (cable replugged, or
             * the host reconfigured usb0): ask for a renew so the address is
             * re-validated instead of being trusted until T1. The event is
             * consumed only while the client is BOUND -- which is exactly the
             * case guarded here; when it is not bound the DORA loop is already
             * retrying on its own and needs no help. */
            if (up && s_dhcp_bound) {
                ca_stdout_put_n("link back UP: requesting lease renew");
                EVAL_DHCP_SVC(ca_dhcp_service_renew();)
            }
            prev = up;
        }
        ca_os_sleep(500);
    }
    return 0U;
}

/* ============================================================================
 * Heartbeat
 * ============================================================================ */
static u32 task_heartbeat(void *arg)
{
    (void)arg;
    ca_led_init();
    while (1) {
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

    /* ca_net_init() is NOT called here (unlike ex35): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */

    /* ARP service: temporary IP 0.0.0.0, updated in on_dhcp_bound */
    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })

    /* DHCP service: DORA starts once task_net opens the net_ready gate */
    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)
    EVAL_UDP_SRV(ca_udp_server_start(&srv_cfg);)

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m70a, "=== Example 70 ===");
PRIVATE_STRING(m70b, "EN_NET / EN_USB / EN_USB_NCM / EN_DHCP_SERVICE == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m70a);
    PUT_N(&priv_m70b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_NET_DHCP && EN_DHCP_SERVICE && EN_UDP_SERVER */
