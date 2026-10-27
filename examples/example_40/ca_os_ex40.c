// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 40 -- caOS TCP Client (HTTP GET)
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically (DORA + T1/T2 renew)
 *   2. ARP service: TCP server MAC resolution
 *   3. TCP client: connection to server, HTTP GET, read response
 *   4. On request only: idle at boot, "e40 start" runs ONE fetch and ends it
 *      with a verdict the bench reads:
 *        RUN RESULT : OK  (1/1)     connected, sent, read a non-empty reply
 *        RUN RESULT : FAIL  (0/1)   any of the three did not happen
 *      It used to loop every 30 s from boot: the bench waited ~29 s for a
 *      cycle to come round and counted events in time windows (04/10).
 *   5. ICMP echo reply (ping from the board)
 *   6. Diagnostics via CLI: "eth pool" | "eth stats" | "tcp list"
 *      "e40 start | info"
 *
 * PC Server: start tcp_server.py in the example directory.
 *   python3 tcp_server.py
 *   (listens on TCP 8080, responds to every GET with a test message)
 *
 * Network flow (task_net):
 *   ca_net_proto_init()
 *   ca_net_proto_bind_udp(68)      -- DHCP OFFER/ACK
 *   ca_eth_link_poll()
 *   ca_dhcp_service_net_ready()    -- unblocks DHCP task
 *
 *   on_dhcp_bound():
 *     updates proto stack + arp + garp
 *     ca_sem_post(&s_dhcp_sem)     -- unblocks task_tcp
 *
 * TCP flow (task_tcp):
 *   waits for s_dhcp_sem
 *   loop:
 *     waits for s_start_sem ("e40 start")
 *     ca_tcp_connect() blocking
 *     ca_tcp_send() HTTP GET
 *     ca_tcp_recv() loop until closure
 *     ca_tcp_close()
 *     RUN RESULT
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_ARP_SERVICE 1
 *   EN_DHCP_SERVICE 1, EN_NET_DHCP 1
 *   EN_TCP_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"

#if EN_ETH && EN_NET && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE

/* ============================================================================
 * Constants
 * ============================================================================ */
#define TCP_SERVER_IP       { 192U, 168U, 10U, 1U }
#define TCP_SERVER_PORT     8080U
#define TCP_RECV_TIMEOUT_MS 10000U

/* ============================================================================
 * Hardware — NUCLEO-H753ZI, RMII
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x40U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static ca_semaphore_t s_dhcp_sem;
static ca_semaphore_t s_start_sem;          /* posted by "e40 start" */
static volatile bool s_busy;             /* a fetch is running */
static volatile bool s_ready;            /* client released by the lease */
static volatile u32  s_runs, s_runs_ok;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x40U } },
    .link_cb  = on_link_change,
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
        ip.v[0],ip.v[1],ip.v[2],ip.v[3]);

    ca_arp_send_garp();

    ca_sem_post(&s_dhcp_sem);   /* unblocks task_tcp */
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
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x40U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX40: TCP client HTTP GET ===");
    ca_stdout_printf_n("MAC          %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    {
        ca_ipv4_t srv = { .v = TCP_SERVER_IP };
        ca_stdout_printf_n("TCP server   %u.%u.%u.%u:%u",
            (u32)srv.v[0],(u32)srv.v[1],(u32)srv.v[2],(u32)srv.v[3],
            (u32)TCP_SERVER_PORT);
    }

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
    s_ready = true;
    /* Idle until asked: say so, or the silence looks like a hang. */
    ca_stdout_put_n("ex40: idle - \"e40 start\" fetches once from the server");

    u32 conn_ok  = 0U;
    u32 conn_err = 0U;

    while (1) {
        ca_sem_wait(&s_start_sem);
        s_busy = true;
        s_runs++;

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
            ca_stdout_put_n("  RUN RESULT : FAIL  (0/1)");
            s_busy = false;
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

        bool ok = tx_ok && total_rx > 0U;
        if (ok) s_runs_ok++;
        ca_stdout_printf_n("  RUN RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
        s_busy = false;
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

/* Stats/pool/leak diagnostics: available on request via CLI "eth stats" |
 * "eth pool" | "eth dump" (caos/drivers/src/eth.c) - no periodic task here. */

/* ============================================================================
 * CLI "e40"
 * ============================================================================ */
#if ENABLE_CLI
static bool ex40_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e40       : start | info");
        return false;
    }
    if (ca_strncmp(p, "e40", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "start", 5) == 0) {
        if (!s_ready)     { CLI_PUT_N("e40: no DHCP lease yet"); return true; }
        if (s_busy)       { CLI_PUT_N("e40: a fetch is already running"); return true; }
        ca_sem_post(&s_start_sem);
        return true;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("client : %s", !s_ready ? "waiting for lease" : (s_busy ? "busy" : "ready"));
        CLI_PRINTF_N("runs   : %u  ok %u", s_runs, s_runs_ok);
        return true;
    }
    CLI_PUT_N("e40: start | info");
    return true;
}

static ca_cli_node_t s_ex40_cli_node = { ex40_cli_handler, NULL };
#endif

/* ============================================================================
 * GPIO ETH — NUCLEO-H753ZI (RMII)
 * ============================================================================ */
static void eth_gpio_init(void)
{
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
    ca_sem_init(&s_start_sem, "E40GO", 0U, 1U);
    EVAL_CLI( ca_cli_register(&s_ex40_cli_node); )
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();

    ca_os_job("net",     CA_TH_LEVEL4, task_net,     NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("tcp",     CA_TH_LEVEL3, task_tcp,     NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m40a, "=== Example 40: caOS TCP Client (HTTP GET) ===");
PRIVATE_STRING(m40b, "DISABLED - requires EN_ETH / EN_NET / EN_ARP_SERVICE / EN_DHCP_SERVICE / EN_NET_DHCP / EN_TCP_SERVICE");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m40a);
    PUT_N(&priv_m40b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE */
