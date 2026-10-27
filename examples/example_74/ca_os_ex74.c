// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 74 -- caOS Syslog Service over USB-NCM
 *
 * NCM variant of example_39 (same service on ETH). The application logic is
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically (DORA + T1/T2 renew)
 *   2. Syslog service: sends log messages to UDP server 514 (RFC 3164)
 *   3. ARP service: syslog server MAC resolution
 *   4. ICMP echo reply (ping from the board)
 *   5. CLI: syslog info, arp info, dhcp info/renew
 *
 * Syslog message sources in the example:
 *   - on_dhcp_bound:    INFO  "EX74 BOUND IP=x.x.x.x"
 *   - on_dhcp_expire:   WARN  "EX74 DHCP lease expired"
 *   - task_ncm_mon:     INFO/WARN  "EX74 USB NCM link UP/DOWN"
 *   - task_heartbeat:   DEBUG "EX74 heartbeat seq=N pool=M"
 *   - task_stats:       INFO  "EX74 stats rx=N tx=N drop=N slg_drop=N"
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side -- the syslog server AND the DHCP server both run on the host:
 *   examples/example_74/test_syslog_ncm_pc.sh
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_NET_SYSLOG 1 | EN_SYSLOG_SERVICE 1 |
 *                           EN_ARP_SERVICE 1
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


#if EN_NET && EN_USB && EN_USB_NCM && EN_NET_SYSLOG && EN_SYSLOG_SERVICE && \
    EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP

/* ============================================================================
 * Constants
 * ============================================================================ */
#define SYSLOG_SERVER_IP     { 192U, 168U, 10U, 1U }  /* PC syslog server IP */
#define HEARTBEAT_PERIOD_MS  30000U
#define STATS_PERIOD_MS      60000U

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x74U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile ca_ipv4_t s_mask       = { .addr = 0U };
static volatile ca_ipv4_t s_gw         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xB4U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * DHCP UDP handler (port 68)
 * ============================================================================ */
static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b   = ca_net_buf_ptr(buf_idx);
    u8           *frm = b->data + b->data_off;
    u16           l4  = b->meta.rx.l4_off;
    const ca_udp_frame_t *udp = (const ca_udp_frame_t *)(frm + l4);
    ca_dhcp_service_rx_handler(udp, (u16)(b->data_len - l4));
    ca_net_buf_free(buf_idx);
}

/* ============================================================================
 * DHCP callbacks
 * ============================================================================ */

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    (void)dns;
    s_ip         = ip;
    s_mask       = mask;
    s_gw         = gw;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  mask=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3],
        mask.v[0], mask.v[1], mask.v[2], mask.v[3]);
    ca_stdout_printf_n("            GW=%u.%u.%u.%u",
        gw.v[0], gw.v[1], gw.v[2], gw.v[3]);

    ca_arp_send_garp();

    /* unlocks the syslog task: from here on the IP is valid.
     * As in ex71/72/73, no NCM link gate of its own is needed: this runs
     * behind DHCP, and a lease cannot exist while the link is down. */
    ca_syslog_service_net_ready();

    /* first message: notify the bind */
    char msg[64];
    ca_snprintf(msg, sizeof(msg),
                "EX74 BOUND IP=%u.%u.%u.%u",
                (u32)ip.v[0], (u32)ip.v[1],
                (u32)ip.v[2], (u32)ip.v[3]);
    ca_syslog_send(SYSLOG_SEV_INFO, msg);
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
    ca_syslog_send(SYSLOG_SEV_WARN, "EX74 DHCP lease expired - rebinding");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },   /* 0.0.0.0 — updated after DHCP */
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x74U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX74: DHCP + Syslog service over USB-NCM ===");
    ca_stdout_printf_n("MAC          %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_put_n("IP           --- (waiting for DHCP)");
    {
        ca_ipv4_t srv = { .v = SYSLOG_SERVER_IP };
        ca_stdout_printf_n("Syslog       -> %u.%u.%u.%u:514",
            (u32)srv.v[0],(u32)srv.v[1],(u32)srv.v[2],(u32)srv.v[3]);
    }

    /* USB NCM bring-up (replaces eth_gpio_init() of ex39) */
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

    /* Register port 68 to receive OFFER/ACK.
     * Syslog has no bind(): it only transmits, it never listens. */
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

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
             *
             * NB ca_dhcp_service_bind() is deliberately NOT called: it blocks,
             * and would stop ca_net_process() from running -> the OFFER would
             * never be processed -> deadlock. The IP arrives through the
             * on_dhcp_bound callback from the DHCP task (LEVEL1). */
            ca_dhcp_service_net_ready();
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

/* ============================================================================
 * Heartbeat task — sends a DEBUG syslog message periodically
 * ============================================================================ */
static u32 task_heartbeat(void *arg)
{
    (void)arg;
    u32 seq = 0U;

    ca_led_init();

    while (1) {
        ca_os_sleep(HEARTBEAT_PERIOD_MS);
        ca_led_toggle(1U);

        if (!s_dhcp_bound) continue;  /* no IP, syslog cannot deliver */

        u16  pool_free = ca_net_buf_free_count();
        char msg[64];
        ca_snprintf(msg, sizeof(msg),
                    "EX74 heartbeat seq=%u pool=%u/%u",
                    (u32)seq, (u32)pool_free, (u32)NET_BUF_COUNT);
        ca_syslog_send(SYSLOG_SEV_DEBUG, msg);
        seq++;
    }
    return 0U;
}

/* ============================================================================
 * Stats task — sends statistics via syslog every STATS_PERIOD_MS
 * ============================================================================ */
static u32 task_stats(void *arg)
{
    (void)arg;
    while (1) {
        ca_os_sleep(STATS_PERIOD_MS);

        ca_net_stats_t st;
        ca_net_stats(&st);
        u32 slg_drop = ca_syslog_dropped();

        /* Local pool diagnostics on request: CLI "net". This task only ships
         * the periodic stats to the remote syslog server. */
        if (s_dhcp_bound) {
            char msg[80];
            ca_snprintf(msg, sizeof(msg),
                        "EX74 stats rx=%u tx=%u drop=%u slg_drop=%u",
                        st.rx_frames, st.tx_frames,
                        st.rx_dropped_pool, slg_drop);
            ca_syslog_send(SYSLOG_SEV_INFO, msg);
        }
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of on_link_change + task_eth_mon
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");

            /* The link syslog is where the queue decoupling shows: the FIRST
             * "link UP" happens before any lease exists. It is NOT lost --
             * ca_syslog_send() only enqueues, and the syslog task is parked on
             * its own gate until ca_syslog_service_net_ready(). The bound
             * message goes through the same call, so both sit in the same FIFO
             * and reach the server in event order (link UP, then BOUND) but
             * with the SAME timestamp: they leave together when the gate opens.
             * slg_drop counts only queue-full losses, so it stays at zero. */
            if (up)
                ca_syslog_send(SYSLOG_SEV_INFO, "EX74 USB NCM link UP");
            else
                ca_syslog_send(SYSLOG_SEV_WARN, "EX74 USB NCM link DOWN");

            /* Coming back up with a lease already taken: ask for a renew so the
             * address is re-validated instead of being trusted until T1. */
            if (up && s_dhcp_bound) {
                ca_stdout_put_n("link back UP: requesting lease renew");
                ca_dhcp_service_renew();
            }
            prev = up;
        }
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

    /* ca_net_init() is NOT called here (unlike ex39): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */

    /* ARP service: temporary IP 0.0.0.0, updated in on_dhcp_bound */
    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    }

    /* DHCP service: DORA starts once task_net opens the net_ready gate */
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);

    /* Syslog service: server fixed, facility local0 as in ex39 */
    {
        ca_ipv4_t srv = { .v = SYSLOG_SERVER_IP };
        ca_syslog_service_init(srv, s_mac, SYSLOG_FAC_LOCAL0);
    }
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_syslog_service_start();

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("stats",     CA_TH_LEVEL3, task_stats,     NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m74a, "=== Example 74 ===");
PRIVATE_STRING(m74b, "EN_NET / EN_USB / EN_USB_NCM / EN_SYSLOG_SERVICE / EN_DHCP_SERVICE == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m74a);
    PUT_N(&priv_m74b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_NET_SYSLOG && ... */
