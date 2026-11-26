// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex149.c
 * @brief          : Example 149 — MicroPython and the caOS network services
 ******************************************************************************
 * @details
 * The network of example 37 (Ethernet, DHCP, ARP, ping, NTP, DNS) plus the
 * syslog client of example 39, and the MicroPython REPL: from a script,
 * caos.net.dns(), ntp_time(), syslog() and ifconfig() use the caOS
 * services; the socket module opens TCP and UDP sockets on tcp_service and
 * udp_client.
 *
 * With EN_MQTT_SERVICE the application owns the MQTT connection (task_mqtt,
 * broker = the bench PC, as in example 42) and hands every message to Python
 * through ca_python_mqtt_message(): a script publishes, subscribes and reads
 * with caos.net.mqtt_publish/mqtt_subscribe/mqtt_recv. With EN_MODBUS_SERVICE
 * and EN_TFTP_SERVICE the script is the client itself: caos.net.modbus_*()
 * opens its own session, caos.net.tftp_get/put() move a file in RAM.
 *
 *   py             MicroPython REPL (Ctrl-D on an empty line exits)
 *   dns, ntp, syslog, dhcp, arp ...   the services' own commands
 ******************************************************************************
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_NET_DHCP && EN_NET_NTP && EN_NET_DNS && EN_NET_SYSLOG && \
    EN_DHCP_SERVICE && EN_NTP_SERVICE && EN_DNS_SERVICE && EN_SYSLOG_SERVICE && EN_PYTHON

/* ============================================================================
 * Constants
 * ============================================================================ */
static const ca_ipv4_t NTP_SERVER_IP_FALLBACK = { .v = { 192U, 168U, 10U, 1U } };
static const ca_ipv4_t DNS_SERVER_IP_FALLBACK = { .v = { 192U, 168U, 10U, 1U } };
static const ca_ipv4_t SYSLOG_SERVER_IP       = { .v = { 192U, 168U, 10U, 1U } };

#if EN_MQTT_SERVICE
#define MQTT_BROKER_PORT     1883U
#define MQTT_CLIENT_ID       "caos-ex149"
#define MQTT_PING_PERIOD_MS  30000U     /* keepalive, < MQTT_KEEPALIVE_SEC */
#define MQTT_RECONNECT_MS    8000U
#endif

/* ============================================================================
 * Hardware
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = CA_EXAMPLE_MAC(149);

static volatile ca_ipv4_t s_ip   = { .addr = 0U };
static volatile ca_ipv4_t s_mask = { .addr = 0U };
static volatile ca_ipv4_t s_gw   = { .addr = 0U };
static volatile ca_ipv4_t s_dns  = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;


static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = CA_EXAMPLE_MAC(149),
    .link_cb  = on_link_change,
};

/* ============================================================================
 * DHCP UDP handler (port 68)
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

#if EN_ARP_SERVICE
    ca_arp_service_update_local_ip(ip);
#endif

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  mask=%u.%u.%u.%u",
        ip.v[0],ip.v[1],ip.v[2],ip.v[3],
        mask.v[0],mask.v[1],mask.v[2],mask.v[3]);
    ca_stdout_printf_n("            GW=%u.%u.%u.%u  DNS=%u.%u.%u.%u",
        gw.v[0],gw.v[1],gw.v[2],gw.v[3],
        dns.v[0],dns.v[1],dns.v[2],dns.v[3]);

    ca_arp_send_garp();

    /* NTP: uses opt42 from DHCP, fallback to gateway */
    EVAL_NTP_SVC({
        ca_ipv4_t ntp_ip = ca_dhcp_service_get_ntp_server();
        if (ntp_ip.addr == 0U) ntp_ip = gw;
        ca_ntp_service_set_server(ntp_ip);
        ca_ntp_service_net_ready();
    })

    /* DNS: uses server from DHCP (opt6), fallback to gateway */
    EVAL_DNS_SVC({
        ca_ipv4_t dns_ip = dns;
        if (dns_ip.addr == 0U) dns_ip = gw;
        ca_dns_service_set_server(dns_ip);
        ca_dns_service_net_ready();
    })

    /* syslog: the collector is the bench PC */
    ca_syslog_service_net_ready();
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED - renewing...");
}

/* ============================================================================
 * NTP callbacks
 * ============================================================================ */
static void unix_to_dt(u32 t, u16 *yr, u8 *mo, u8 *dy, u8 *hh, u8 *mm, u8 *ss)
{
    static const u8 mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

    *ss = (u8)(t % 60U); t /= 60U;
    *mm = (u8)(t % 60U); t /= 60U;
    *hh = (u8)(t % 24U); t /= 24U;

    u32 days = t;
    u32 y = 1970U;
    while (1) {
        bool leap = ((y % 4U == 0U) && (y % 100U != 0U)) || (y % 400U == 0U);
        u32 dpy = leap ? 366U : 365U;
        if (days < dpy) break;
        days -= dpy;
        y++;
    }
    *yr = (u16)y;

    bool leap = ((*yr % 4U == 0U) && (*yr % 100U != 0U)) || (*yr % 400U == 0U);
    u8 m = 1U;
    for (; m <= 12U; m++) {
        u8 md = mdays[m - 1U];
        if (leap && m == 2U) md = 29U;
        if (days < (u32)md) break;
        days -= (u32)md;
    }
    *mo = m;
    *dy = (u8)(days + 1U);
}

static void on_ntp_sync(u32 unix_time)
{
    u16 yr; u8 mo, dy, hh, mm, ss;
    unix_to_dt(unix_time, &yr, &mo, &dy, &hh, &mm, &ss);
    ca_stdout_printf_n(
        "NTP SYNC  unix=%u  %04u-%02u-%02u %02u:%02u:%02u UTC",
        (unsigned long)unix_time,
        (u32)yr, (u32)mo, (u32)dy,
        (u32)hh, (u32)mm, (u32)ss);
}

static void on_ntp_fail(void)
{
    ca_stdout_put_n("NTP FAIL  server unreachable");
}

/* ============================================================================
 * Network helpers
 * ============================================================================ */

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = CA_EXAMPLE_MAC(149),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX149: MicroPython + DHCP + NTP + DNS + syslog ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_put_n("IP   --- (waiting for DHCP)");

    ca_net_proto_init(&net_cfg);

    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    EVAL_NTP_SVC(ca_ntp_service_bind();)
    EVAL_DNS_SVC(ca_dns_service_bind();)

    ca_eth_link_poll();
    ca_os_sleep(200);

    EVAL_DHCP_SVC(ca_dhcp_service_net_ready();)
    ca_stdout_put_n("DHCP: starting DORA...");

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

#if EN_MQTT_SERVICE
/* ============================================================================
 * MQTT - the application's connection, the messages go to Python
 * ============================================================================ */
/* Silent on purpose: the connection comes and goes at any moment, and the
 * console may belong to a Python script by then - a line printed here lands
 * in the middle of the REPL's output (seen on the bench, 24/11). Python asks
 * caos.net.mqtt_connected() instead. */
static void on_mqtt_connected(void)    { }
static void on_mqtt_disconnected(void) { }

static void mqtt_start(void)
{
    /* the broker is the bench PC, the gateway of the lease */
    ca_mqtt_service_init(s_gw, MQTT_BROKER_PORT, s_ip, s_mac, MQTT_CLIENT_ID,
                         on_mqtt_connected, ca_python_mqtt_message,
                         on_mqtt_disconnected);
    (void)ca_mqtt_service_net_ready();
}

static u32 task_mqtt(void *arg)
{
    (void)arg;
    while (!s_dhcp_bound) ca_os_sleep(200U);
    mqtt_start();
    u32 last_try  = ca_os_tick_ms();
    u32 last_ping = last_try;
    while (1) {
        const u32 now = ca_os_tick_ms();
        if (!ca_mqtt_is_connected()) {
            if (s_dhcp_bound && (now - last_try) >= MQTT_RECONNECT_MS) {
                last_try = now;
                mqtt_start();
            }
        } else if ((now - last_ping) >= MQTT_PING_PERIOD_MS) {
            last_ping = now;
            ca_mqtt_ping();
        }
        ca_os_sleep(500U);
    }
    return 0U;
}
#endif

/* Stats/pool/leak diagnostics: available on request via CLI "ip stats"
 * (caos/net) | "eth pool" | "eth dump" (caos/drivers/src/eth.c) — no periodic task here. */

/* ============================================================================
 * Link / heartbeat / ETH GPIO
 * ============================================================================ */
static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) { ca_eth_link_poll(); ca_os_sleep(500); }
    return 0U;
}

static u32 task_heartbeat(void *arg)
{
    (void)arg;
    while (1) { ca_led_toggle(3U); ca_os_sleep(500); }
    return 0U;
}

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
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg))) {
        CA_SAFE_FAIL("ca_net_init");
    }

    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })

    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)

    EVAL_NTP_SVC(ca_ntp_service_init(NTP_SERVER_IP_FALLBACK, s_mac, on_ntp_sync, on_ntp_fail);)

    EVAL_DNS_SVC(ca_dns_service_init(DNS_SERVER_IP_FALLBACK, s_mac);)

    ca_syslog_service_init(SYSLOG_SERVER_IP, s_mac, SYSLOG_FAC_LOCAL0);

    EVAL_TFTP_SVC(ca_tftp_service_init(s_mac);)

    /* socket module: the MAC of the services above; the address comes from
     * the lease (TCP with EN_TCP_SERVICE, UDP with EN_UDP_CLIENT) */
    ca_python_set_mac(s_mac);
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)
    EVAL_NTP_SVC(ca_ntp_service_start();)
    EVAL_DNS_SVC(ca_dns_service_start();)
    ca_syslog_service_start();
    EVAL_TCP_SVC(ca_tcp_service_start();)

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
#if EN_MQTT_SERVICE
    ca_os_job("mqtt",      CA_TH_LEVEL3, task_mqtt,      NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
#endif
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m149a, "=== Example 149 ===");
PRIVATE_STRING(m149b, "ex149: needs EN_ETH, EN_NET, DHCP, NTP, DNS, syslog and EN_PYTHON");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m149a);
    PUT_N(&priv_m149b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_NET_DHCP && EN_NET_NTP && EN_NET_DNS && ... */
