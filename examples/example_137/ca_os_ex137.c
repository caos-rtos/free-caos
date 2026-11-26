// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 137 -- caOS TCP over a reduced MTU (clone of ex41 + EN_IP_FRAG)
 *
 * Hardware: any Nucleo with Ethernet (H753, F767, H563, F439, H773 ...)
 *
 * The question this example answers: what happens to a TCP connection that
 * is ALREADY open when the MTU of its interface goes down ('ip mtu 512',
 * a radio link). A new connection takes its MSS from the MTU (local_mss()),
 * but one opened at 1500 negotiated MSS 1460: its segments leave with DF
 * set and are bigger than the new MTU. The ip_frag service does not fragment
 * our own TCP (DF), it drops them and counts 'tcp_oversize' in 'ip stats'.
 *
 * Functionality:
 *   1. DHCP client, ARP service, ICMP echo reply
 *   2. TCP echo server on port 7 (RFC 862), SILENT: one line per session
 *      (ex41 prints every chunk, binary included)
 *   3. ip_frag service: 'ip mtu N' on the CLI shrinks the MTU of iface 0
 *   4. Diagnostics: "ip stats" (frag counters, tcp_oversize) | "tcp list"
 *
 * Test from PC (examples/example_137/tcp_mtu_drop.py):
 *   phase A: connect at MTU 1500, echo; phase B: the board goes to 'ip mtu
 *   512' on the SAME connection, echo again; phase C: a NEW connection at
 *   512 must get MSS 472 and echo fine.
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_ARP_SERVICE 1, EN_DHCP_SERVICE 1, EN_NET_DHCP 1,
 *   EN_TCP_SERVICE 1, EN_IP_FRAG 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"

#if EN_ETH && EN_NET && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_IP_FRAG

/* ============================================================================
 * Constants
 * ============================================================================ */
#define TCP_ECHO_PORT       7U
#define ECHO_RECV_TIMEOUT   5000U

/* ============================================================================
 * Hardware — NUCLEO-H753ZI, RMII
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = CA_EXAMPLE_MAC(137);

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static ca_semaphore_t s_dhcp_sem;
static u8          s_lh = 0U;    /* listen handle */

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = CA_EXAMPLE_MAC(137),
    .link_cb  = on_link_change,
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
        ip.v[0],ip.v[1],ip.v[2],ip.v[3]);
    ca_stdout_printf_n("TCP echo server on port %u", (u32)TCP_ECHO_PORT);

    ca_arp_send_garp();

    /* updates the listen PCB with the real IP */
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

    ca_sem_post(&s_dhcp_sem);
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * Link callback
 * ============================================================================ */
static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = CA_EXAMPLE_MAC(137),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX137: TCP over a reduced MTU ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("Port    TCP:%u  (RFC 862 echo)", (u32)TCP_ECHO_PORT);

    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    ca_eth_link_poll();
    ca_os_sleep(200);

    ca_dhcp_service_net_ready();
    ca_stdout_put_n("DHCP: starting DORA...");

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

/* ============================================================================
 * TCP echo server task
 * ============================================================================ */
static u32 task_server(void *arg)
{
    (void)arg;

    /* One full MSS at MTU 1500: the echo goes out in segments as big as
     * the connection allows, which is what the MTU drop has to meet. */
    static u8 buf[1460];

    u32 total_accepted = 0U;
    u32 total_bytes    = 0U;

    /* waits for DHCP bind (s_lh updated with real IP in on_dhcp_bound) */
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
            bool ok  = ca_tcp_recv(h, buf, (u16)sizeof(buf),
                                   &got, ECHO_RECV_TIMEOUT);
            if (!ok) {
                /* timeout: if still connected continue waiting */
                if (ca_tcp_is_connected(h)) continue;
                break;  /* peer closed */
            }
            if (got == 0U) continue;

            /* The whole chunk in one call: ca_tcp_send() is all or nothing,
             * so CA_TCP_RETRY means nothing went - wait for the slots it
             * needs and send the same again. (Before 17/11 a buffer of
             * several segments could stop half way with RETRY, and after an
             * 'ip mtu' drop 1460 B are 4 segments.) */
            bool gone = false;
            u32  tries = 0U;
            for (;;) {
                const i32 r = ca_tcp_send(h, buf, got);
                if (r == CA_TCP_SENT) break;
                if (r == CA_TCP_GONE || ++tries > 1000U) { gone = true; break; }
                if (!ca_tcp_wait_writable(h, 1000U) && !ca_tcp_is_connected(h)) {
                    gone = true; break;
                }
            }
            if (gone) break;
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
 * ETH monitor
 * ============================================================================ */
static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) {
        ca_eth_link_poll();
        ca_led_toggle(3U);
        ca_os_sleep(500);
    }
    return 0U;
}

/* Stats/pool/leak diagnostics: available on request via CLI "ip stats"
 * (caos/net) | "eth pool" | "eth dump" (caos/drivers/src/eth.c) - no periodic task here. */

/* ============================================================================
 * GPIO ETH — NUCLEO-H753ZI (RMII)
 * ============================================================================ */
static void eth_gpio_init(void)
{
#if MCU_ID == 773
    /* NUCLEO-H7S3L8 (UM3276 7.13, DS14359 AF11): REF_CLK PB6, MDIO PA2,
     * MDC PG6, CRS_DV PA7, RXD0/RXD1 PG4/PG5, TX_EN PG11, TXD0/TXD1 PG13/PG12 */
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_B,  6U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_G,  6U },
        { CA_GPIO_PORT_A,  7U }, { CA_GPIO_PORT_G,  4U }, { CA_GPIO_PORT_G,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U }, { CA_GPIO_PORT_G, 12U },
    };
#else
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
        /* RMII_TXD1: NUCLEO-H563ZI (UM3115) uses PB15 (JP6 ON, SB10 OFF);
         * NUCLEO-H753ZI uses PB13. Hardcoding PB13 leaves TXD1 unconnected
         * on the H563: the MAC still counts every frame as transmitted and
         * tx_fail stays 0, but half the data bits never reach the PHY and
         * nothing valid appears on the wire. */
#if MCU_ID == 563
        { CA_GPIO_PORT_B, 15U },
#else
        { CA_GPIO_PORT_B, 13U },
#endif
    };
#endif
    for (u32 i = 0U; i < sizeof(pins)/sizeof(pins[0]); i++) {
        ca_gpio_configure(pins[i].port, pins[i].pin,
            CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
            CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, ETH_AF);
    }
}

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
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");

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
    ca_ip_frag_service_start();     /* 'ip mtu N' diverts what is over N */

    /* Listen with temporary IP=0.0.0.0: on_dhcp_bound will update it */
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
    }

    ca_os_job("net",     CA_TH_LEVEL4, task_net,    NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon,NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("server",  CA_TH_LEVEL3, task_server, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m137a, "=== Example 137: caOS TCP over a reduced MTU ===");
PRIVATE_STRING(m137b, "DISABLED - requires EN_ETH / EN_NET / EN_ARP_SERVICE / EN_DHCP_SERVICE / EN_NET_DHCP / EN_TCP_SERVICE / EN_IP_FRAG");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m137a);
    PUT_N(&priv_m137b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_IP_FRAG */
