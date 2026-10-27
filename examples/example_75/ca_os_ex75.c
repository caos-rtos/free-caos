// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 75 -- caOS TCP Client (HTTP GET) over USB-NCM
 *
 * NCM variant of example_40 (same service on ETH). The application logic is
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically (DORA + T1/T2 renew)
 *   2. ARP service: TCP server MAC resolution
 *   3. TCP client: connection to server, HTTP GET, read response
 *   4. Loop: reconnects every TCP_RETRY_PERIOD_MS after closure/error
 *   5. ICMP echo reply (ping from the board)
 *   6. Diagnostics via CLI: "tcp list" | "net"
 *
 * First TCP clone of the NCM series: everything before it was UDP.
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side -- the DHCP server AND the TCP server both run on the host:
 *   sudo examples/example_75/test_tcp_ncm_pc.sh
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
#define TCP_SERVER_IP       { 192U, 168U, 10U, 1U }   /* the PC, on a p2p link */
#define TCP_SERVER_PORT     8080U
#define TCP_RETRY_PERIOD_MS 30000U
#define TCP_RECV_TIMEOUT_MS 10000U

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x75U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static ca_semaphore_t s_dhcp_sem;   /* on_dhcp_bound → task_tcp */

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xB5U },  /* Host-side MAC */
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
    (void)mask; (void)gw; (void)dns;
    s_ip         = ip;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3]);

    ca_arp_send_garp();

    /* Unblocks task_tcp. The trywait before the post is not cosmetic: the
     * semaphore has max_count 1 and ca_sem_post() trips CA_SAFEGUARD on
     * overflow, so a renew arriving before task_tcp has consumed the token
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
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x75U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX75: TCP client HTTP GET over USB-NCM ===");
    ca_stdout_printf_n("MAC          %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    {
        ca_ipv4_t srv = { .v = TCP_SERVER_IP };
        ca_stdout_printf_n("TCP server   %u.%u.%u.%u:%u",
            (u32)srv.v[0],(u32)srv.v[1],(u32)srv.v[2],(u32)srv.v[3],
            (u32)TCP_SERVER_PORT);
    }

    /* USB NCM bring-up (replaces eth_gpio_init() of ex40) */
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

    /* Register port 68 to receive OFFER/ACK. TCP needs no bind here: the
     * client opens its own connection, it does not listen. */
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
     * link is seen up.
     *
     * The TCP client needs no gate of its own: it waits on s_dhcp_sem, posted
     * from on_dhcp_bound, and a lease cannot exist while the link is down. */
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
 * TCP client task — HTTP GET loop
 * ============================================================================ */
static u32 task_tcp(void *arg)
{
    (void)arg;

    static const char http_get[] =
        "GET / HTTP/1.0\r\n"
        "Host: caOS-board\r\n"
        "Connection: close\r\n"
        "\r\n";

    static u8 rx_buf[512];

    /* waits for DHCP bind */
    ca_sem_wait(&s_dhcp_sem);
    ca_stdout_put_n("TCP: DHCP bound, starting client loop");

    u32 conn_ok  = 0U;
    u32 conn_err = 0U;

    while (1) {
        ca_ipv4_t srv_ip  = { .v = TCP_SERVER_IP };
        ca_ipv4_t local   = s_ip;

        ca_stdout_printf_n("\nTCP connect -> %u.%u.%u.%u:%u",
            (u32)srv_ip.v[0],(u32)srv_ip.v[1],
            (u32)srv_ip.v[2],(u32)srv_ip.v[3],
            (u32)TCP_SERVER_PORT);

        ca_tcp_connect_cfg_t cfg = {
            .remote_ip          = srv_ip,
            .remote_port        = TCP_SERVER_PORT,
            .local_ip           = local,
            .local_mac          = s_mac,
            .arp_timeout_ms     = 0U,   /* uses default TCP_ARP_TIMEOUT_MS */
            .connect_timeout_ms = 0U,   /* uses default TCP_CONNECT_TIMEOUT_MS */
            .on_rx              = NULL, /* blocking mode ca_tcp_recv() */
            .on_connected       = NULL, /* blocks on connect */
            .on_closed          = NULL,
        };

        u8 h = ca_tcp_connect(&cfg);
        if (h == 0U) {
            conn_err++;
            ca_stdout_printf_n("TCP connect FAILED  err=%u", (u32)conn_err);
            ca_os_sleep(TCP_RETRY_PERIOD_MS);
            continue;
        }

        conn_ok++;
        ca_stdout_printf_n("TCP ESTABLISHED  h=%u  ok=%u", (u32)h, (u32)conn_ok);

        /* sends HTTP GET */
        /* == CA_TCP_SENT, not a bare assignment: the return is i32 and
         * CA_TCP_GONE is -1, which is truthy - a failed send would print OK. */
        bool tx_ok = (ca_tcp_send(h, (const u8 *)http_get,
                                  (u16)(sizeof(http_get) - 1U)) == CA_TCP_SENT);
        ca_stdout_printf_n("TCP send GET  %s", tx_ok ? "OK" : "FAIL");

        /* reads response until connection closure */
        u32 total_rx = 0U;
        while (ca_tcp_is_connected(h)) {
            u16 got = 0U;
            bool ok = ca_tcp_recv(h, rx_buf, (u16)(sizeof(rx_buf) - 1U),
                                  &got, TCP_RECV_TIMEOUT_MS);
            if (!ok || got == 0U) break;
            total_rx += got;
            rx_buf[got] = 0U;
            ca_stdout_printf_n("TCP rx %u byte:", (u32)got);
            ca_stdout_put_n((cstr)rx_buf);
        }

        ca_stdout_printf_n("TCP session done  total_rx=%u byte", total_rx);
        ca_tcp_close(h);

        ca_os_sleep(TCP_RETRY_PERIOD_MS);
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex40
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
             * address is re-validated instead of being trusted until T1. */
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

    ca_sem_init(&s_dhcp_sem, "DHCPBND", 0U, 1U);

    /* ca_net_init() is NOT called here (unlike ex40): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */

    /* ARP service: temporary IP 0.0.0.0, updated in on_dhcp_bound */
    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    }

    /* DHCP service: DORA starts once task_net opens the net_ready gate */
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("tcp",       CA_TH_LEVEL3, task_tcp,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m75a, "=== Example 75 ===");
PRIVATE_STRING(m75b, "EN_NET / EN_USB / EN_USB_NCM / EN_TCP_SERVICE / EN_DHCP_SERVICE == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m75a);
    PUT_N(&priv_m75b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_TCP_SERVICE && ... */
