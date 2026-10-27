// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 39 -- caOS Syslog Service
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically (DORA + T1/T2 renew)
 *   2. Syslog service: sends log messages to UDP server 514 (RFC 3164)
 *   3. ARP service: syslog server MAC resolution
 *   4. ICMP echo reply (ping from the board)
 *   5. CLI: syslog info, arp info, dhcp info/renew
 *
 * Syslog message sources in the example:
 *   - on_dhcp_bound:    INFO  "EX39 BOUND IP=x.x.x.x"
 *   - on_dhcp_expire:   WARN  "EX39 DHCP lease expired"
 *   - on_link_change:   INFO/WARN  "EX39 ETH link UP/DOWN"
 *   - task_heartbeat:   DEBUG "EX39 heartbeat seq=N pool=M"
 *   - task_stats:       INFO  "EX39 stats rx=N tx=N drop=N syslog_drop=N"
 *
 * PC syslog server: run syslog_server.py in the example directory.
 *   python3 syslog_server.py
 *   (listens on UDP 514, prints received messages)
 *
 * Network flow (task_net):
 *   ca_net_proto_init()
 *   ca_net_proto_bind_udp(68)     -- DHCP OFFER/ACK
 *   ca_eth_link_poll()
 *   ca_dhcp_service_net_ready()   -- unlocks DHCP task
 *
 *   on_dhcp_bound():
 *     updates proto stack + arp
 *     ca_syslog_service_net_ready()  -- unlocks syslog task
 *     ca_syslog_send(INFO, "EX39 BOUND IP=...")
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_NET_SYSLOG 1
 *   EN_ARP_SERVICE 1, EN_DHCP_SERVICE 1, EN_NET_DHCP 1
 *   EN_SYSLOG_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_NET_SYSLOG && EN_SYSLOG_SERVICE && \
    EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP

/* ============================================================================
 * Constants
 * ============================================================================ */
#define SYSLOG_SERVER_IP   { 192U, 168U, 10U, 1U }  /* PC syslog server IP */
#define HEARTBEAT_PERIOD_MS  30000U
#define STATS_PERIOD_MS      60000U

/* ============================================================================
 * Hardware — NUCLEO-H753ZI, same pins as previous examples
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x39U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile ca_ipv4_t s_mask       = { .addr = 0U };
static volatile ca_ipv4_t s_gw         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x39U } },
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
    (void)dns;
    s_ip         = ip;
    s_mask       = mask;
    s_gw         = gw;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  mask=%u.%u.%u.%u",
        ip.v[0],ip.v[1],ip.v[2],ip.v[3],
        mask.v[0],mask.v[1],mask.v[2],mask.v[3]);
    ca_stdout_printf_n("            GW=%u.%u.%u.%u",
        gw.v[0],gw.v[1],gw.v[2],gw.v[3]);

    ca_arp_send_garp();

    /* unlocks the syslog task: from here on the IP is valid */
    ca_syslog_service_net_ready();

    /* first message: notify the bind */
    char msg[64];
    ca_snprintf(msg, sizeof(msg),
                "EX39 BOUND IP=%u.%u.%u.%u",
                (u32)ip.v[0], (u32)ip.v[1],
                (u32)ip.v[2], (u32)ip.v[3]);
    ca_syslog_send(SYSLOG_SEV_INFO, msg);
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
    ca_syslog_send(SYSLOG_SEV_WARN, "EX39 DHCP lease expired - rebinding");
}

/* ============================================================================
 * Link callback — signals link change via syslog
 * ============================================================================ */
static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
    if (up)
        ca_syslog_send(SYSLOG_SEV_INFO,  "EX39 ETH link UP");
    else
        ca_syslog_send(SYSLOG_SEV_WARN,  "EX39 ETH link DOWN");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x39U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX39: DHCP + Syslog service ===");
    ca_stdout_printf_n("MAC          %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_put_n("IP           --- (waiting for DHCP)");
    {
        ca_ipv4_t srv = { .v = SYSLOG_SERVER_IP };
        ca_stdout_printf_n("Syslog       -> %u.%u.%u.%u:514",
            (u32)srv.v[0],(u32)srv.v[1],(u32)srv.v[2],(u32)srv.v[3]);
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
 * Heartbeat task — sends a DEBUG syslog message periodically
 * ============================================================================ */
static u32 task_heartbeat(void *arg)
{
    (void)arg;
    u32 seq = 0U;
    while (1) {
        ca_os_sleep(HEARTBEAT_PERIOD_MS);
        ca_led_toggle(3U);

        if (!s_dhcp_bound) continue;  /* no IP, syslog cannot deliver */

        u16  pool_free = ca_net_buf_free_count();
        char msg[64];
        ca_snprintf(msg, sizeof(msg),
                    "EX39 heartbeat seq=%u pool=%u/%u",
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

        /* Local pool/leak diagnostics on request: CLI "eth pool" | "eth stats".
         * This task only ships the periodic stats to the remote syslog server. */
        if (s_dhcp_bound) {
            char msg[80];
            ca_snprintf(msg, sizeof(msg),
                        "EX39 stats rx=%u tx=%u drop=%u slg_drop=%u",
                        st.rx_frames, st.tx_frames,
                        st.rx_dropped_pool, slg_drop);
            ca_syslog_send(SYSLOG_SEV_INFO, msg);
        }
    }
    return 0U;
}

/* ============================================================================
 * ETH monitor
 * ============================================================================ */
static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) { ca_eth_link_poll(); ca_os_sleep(500); }
    return 0U;
}

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

    ca_os_job("net",       CA_TH_LEVEL4, task_net,      NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,  NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL3, task_heartbeat,NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("stats",     CA_TH_LEVEL4, task_stats,    NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m39a, "=== Example 39 ===");
/* Split in two: PRIVATE_STRING holds DEFAULT_MSG_SIZE-2 = 130 chars, and the
 * single line listing every flag was 139 (it only failed to build once ex39
 * was compiled for a no-ETH part). */
PRIVATE_STRING(m39b, "EN_ETH / EN_NET / EN_NET_DHCP / EN_DHCP_SERVICE / EN_UDP_SERVER");
PRIVATE_STRING(m39c, "EN_NTP / EN_DNS / EN_NET_SYSLOG / EN_SYSLOG_SERVICE: no ETH tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m39a);
    PUT_N(&priv_m39b);
    PUT_N(&priv_m39c);
}
void ca_os_main(void) { ca_os_run(true); }


#endif /* EN_ETH && EN_NET && EN_NET_SYSLOG && EN_SYSLOG_SERVICE && ... */
