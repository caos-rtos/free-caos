// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 76 -- caOS TCP Server (passive open / echo server) over USB-NCM
 *
 * NCM variant of example_41 (same service on ETH). The application logic is
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically (DORA + T1/T2 renew)
 *   2. ARP service: client MAC resolution
 *   3. TCP server on port 7: echo server (RFC 862)
 *      - accepts one connection at a time (blocking ca_tcp_accept)
 *      - receives data and sends it back (echo)
 *      - closes the connection when the client closes
 *   4. ICMP echo reply (ping from the board)
 *   5. Diagnostics via CLI: "tcp list" | "net"
 *
 * Together with ex75 (TCP client) this covers TCP in both directions on NCM.
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side -- DHCP server, then connect to the board:
 *   sudo python3 examples/example_76/tcp_client_ncm.py
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_TCP_SERVICE 1 | EN_ARP_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"

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


#if EN_NET && EN_USB && EN_USB_NCM && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE

/* ============================================================================
 * Constants
 * ============================================================================ */
#define TCP_ECHO_PORT       7U
#define ECHO_RECV_TIMEOUT   5000U

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x76U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static ca_semaphore_t s_dhcp_sem;
static u8          s_lh = 0U;    /* listen handle */

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xB6U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * DHCP handler UDP (port 68)
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
    (void)mask; (void)gw; (void)dns;
    s_ip         = ip;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    ca_stdout_printf_n("TCP echo server on port %u", (u32)TCP_ECHO_PORT);

    ca_arp_send_garp();

    /* Re-arm the listen PCB with the real IP (it was opened on 0.0.0.0). */
    if (s_lh != 0U) {
        ca_tcp_unlisten(s_lh);
        ca_tcp_listen_cfg_t lcfg = {
            .local_port = TCP_ECHO_PORT,
            .local_ip   = ip,
            .local_mac  = s_mac,
            .on_accept  = NULL,
            .on_rx      = NULL,
            .on_closed  = NULL,
        };
        s_lh = ca_tcp_listen(&lcfg);
    }

    /* Unblocks task_server. The trywait before the post is not cosmetic: the
     * semaphore has max_count 1 and ca_sem_post() trips CA_SAFEGUARD on
     * overflow, so a renew arriving before task_server has consumed the token
     * would halt the system. Draining first makes the pair idempotent. */
    ca_sem_trywait(&s_dhcp_sem);
    ca_sem_post(&s_dhcp_sem);
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },   /* 0.0.0.0 — updated after DHCP */
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x76U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX76: TCP echo server over USB-NCM ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("Port    TCP:%u  (RFC 862 echo)", (u32)TCP_ECHO_PORT);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex41) */
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

    /* Open the listening socket on 0.0.0.0; on_dhcp_bound re-arms it with the
     * leased address.
     *
     * NB this differs from ex41, where ca_tcp_listen() sits in ca_os_main()
     * before ca_os_run(). It cannot stay there on NCM: the net core is not
     * initialised until ca_net_init() runs, and that happens here in task_net
     * because the NCM iface only exists after USB enumeration. Listening is
     * passive -- it puts nothing on the wire -- so it belongs before the gate,
     * like every other bind. */
    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_tcp_listen_cfg_t lcfg = {
            .local_port = TCP_ECHO_PORT,
            .local_ip   = zero,
            .local_mac  = s_mac,
            .on_accept  = NULL,
            .on_rx      = NULL,
            .on_closed  = NULL,
        };
        s_lh = ca_tcp_listen(&lcfg);
        if (s_lh == 0U) ca_stdout_put_n("[TCP] listen FAILED");
    }

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
     * link is seen up.
     *
     * The echo server needs no gate: it only ever answers, and by the time a
     * client can reach it the link is necessarily up. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            /* NB ca_dhcp_service_bind() is deliberately NOT called: it blocks,
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
 * TCP echo server task
 * ============================================================================ */
static u32 task_server(void *arg)
{
    (void)arg;

    static u8 buf[512];

    u32 total_accepted = 0U;
    u32 total_bytes    = 0U;

    /* waits for DHCP bind (s_lh re-armed with the real IP in on_dhcp_bound) */
    ca_sem_wait(&s_dhcp_sem);
    ca_stdout_printf_n("TCP server: listening on port %u", (u32)TCP_ECHO_PORT);

    while (1) {
        u8 h = ca_tcp_accept(s_lh, TCP_TCP_ACCEPT_TIMEOUT_MS);
        if (h == 0U) continue;   /* timeout, retry */

        total_accepted++;
        ca_stdout_printf_n("TCP accept  h=%u  conn#%u", (u32)h, total_accepted);

        u32 session_bytes = 0U;

        while (ca_tcp_is_connected(h)) {
            u16  got = 0U;
            bool ok  = ca_tcp_recv(h, buf, (u16)(sizeof(buf) - 1U),
                                   &got, ECHO_RECV_TIMEOUT);
            if (!ok) {
                /* timeout: if still connected continue waiting */
                if (ca_tcp_is_connected(h)) continue;
                break;  /* peer closed */
            }
            if (got == 0U) continue;

            buf[got] = 0U;
            ca_stdout_printf_n("  echo %u byte: %s", (u32)got, (cstr)buf);

            ca_tcp_send(h, buf, got);
            session_bytes += got;
        }

        total_bytes += session_bytes;
        ca_stdout_printf_n("TCP close  h=%u  session=%u byte  total=%u byte",
            (u32)h, session_bytes, total_bytes);
        ca_tcp_close(h);
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex41
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    ca_led_init();

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            /* Coming back up with a lease already taken: ask for a renew so the
             * address is re-validated. It also re-arms the listen PCB, since
             * that happens inside on_dhcp_bound. */
            if (up && s_dhcp_bound) {
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

/* Pool/stats diagnostics on request via CLI "net" and "tcp list": the "eth"
 * commands live inside #if EN_ETH and are not built on NCM. */

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

    /* ca_net_init() is NOT called here (unlike ex41): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */

    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    }
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);

    ca_sem_init(&s_dhcp_sem, "DHCPBND", 0U, 1U);
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();

    /* ca_tcp_listen() is NOT here (unlike ex41): it needs the net core, which
     * is initialised in task_net after USB enumeration. See there. */

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("server",    CA_TH_LEVEL3, task_server,    NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m76a, "=== Example 76 ===");
PRIVATE_STRING(m76b, "EN_NET / EN_USB / EN_USB_NCM / EN_TCP_SERVICE / EN_DHCP_SERVICE == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m76a);
    PUT_N(&priv_m76b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_TCP_SERVICE && ... */
