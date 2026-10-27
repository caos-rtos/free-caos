// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 36 -- caOS DHCP + ARP + ICMP ping + UDP server + NTP client
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Features:
 *   1. DHCP client: obtains IP automatically (DORA + T1/T2 renew)
 *   2. NTP client: synchronizes unix time with NTP server (port 123)
 *   3. ARP service: cache + blocking resolve
 *   4. ICMP echo reply (ping)
 *   5. UDP server on port 7961 — max 2 simultaneous clients
 *   6. GARP sent after every DHCP bound/renew
 *   7. CLI: dhcp info/renew, ntp info/sync, udp stats/list/kick/send
 *
 * Network flow (task_net):
 *   ca_net_proto_init()               -- MAC known, IP = 0.0.0.0
 *   ca_net_proto_bind_udp(68)         -- DHCP OFFER/ACK
 *   ca_udp_server_bind()              -- port 7961
 *   ca_ntp_service_bind()             -- port 123
 *   ca_dhcp_service_net_ready()       -- unblocks DHCP task
 *   [loop: ca_net_process()]
 *
 *   on_dhcp_bound():
 *     updates proto stack + udp server + arp cache
 *     ca_ntp_service_net_ready()      -- unblocks NTP task (IP valid now)
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_NET_DHCP 1, EN_NET_NTP 1,
 *   EN_ARP_SERVICE 1, EN_UDP_SERVER 1, EN_DHCP_SERVICE 1, EN_NTP_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_NET_DHCP && EN_NET_NTP && \
    EN_DHCP_SERVICE && EN_NTP_SERVICE && EN_UDP_SERVER

/* ============================================================================
 * Constants
 * ============================================================================ */
#define UDP_SRV_PORT       7961U
#define MAX_CLIENTS        2U
#define CLIENT_TIMEOUT_MS  15000U

/* NTP server: taken from DHCP opt42; fallback to gateway if server does not send it */
static const ca_ipv4_t NTP_SERVER_IP_FALLBACK = { .v = { 192U, 168U, 10U, 1U } };

/* ============================================================================
 * Hardware
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x36U } };

static volatile ca_ipv4_t s_ip   = { .addr = 0U };
static volatile ca_ipv4_t s_mask = { .addr = 0U };
static volatile ca_ipv4_t s_gw   = { .addr = 0U };
static volatile ca_ipv4_t s_dns  = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x36U } },
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
    EVAL_UDP_SRV(ca_udp_server_set_local_ip(ip);)

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

    /* Update NTP IP: use opt42 from DHCP, fallback to gateway */
    EVAL_NTP_SVC({
        ca_ipv4_t ntp_ip = ca_dhcp_service_get_ntp_server();
        if (ntp_ip.addr == 0U) ntp_ip = gw;
        ca_ntp_service_set_server(ntp_ip);
        ca_ntp_service_net_ready();
    })
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

/* Converts unix timestamp to readable date/time */
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
 * UDP server callbacks
 * ============================================================================ */
static void on_connect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] client %u: %u.%u.%u.%u:%u connected",
        (u32)cid, ip.v[0],ip.v[1],ip.v[2],ip.v[3], (u32)port);
}

static void on_disconnect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] client %u: %u.%u.%u.%u:%u disconnected",
        (u32)cid, ip.v[0],ip.v[1],ip.v[2],ip.v[3], (u32)port);
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
        ip.v[0],ip.v[1],ip.v[2],ip.v[3], (u32)port);

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

static ca_udp_server_cfg_t srv_cfg = {
    .port              = UDP_SRV_PORT,
    .max_clients       = MAX_CLIENTS,
    .client_timeout_ms = CLIENT_TIMEOUT_MS,
    .local_mac         = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x36U } },
    .local_ip          = { .addr = 0U },
    .on_rx             = on_rx,
    .on_connect        = on_connect,
    .on_disconnect     = on_disconnect,
};

/* ============================================================================
 * Network helpers
 * ============================================================================ */

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x36U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX36: DHCP + NTP + ARP + ping + UDP server ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_put_n("IP   --- (waiting for DHCP)");

    ca_net_proto_init(&net_cfg);

    /* Port 68: DHCP OFFER/ACK */
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    /* Port 7961: UDP server */
    EVAL_UDP_SRV(ca_udp_server_bind();)

    /* Port 123: NTP response
     * The NTP task is unblocked only after DHCP bound (on_dhcp_bound →
     * ca_ntp_service_net_ready), so the IP is already valid at the first request. */
    EVAL_NTP_SVC(ca_ntp_service_bind();)

    ca_eth_link_poll();
    ca_os_sleep(200);

    /* Unblock the DHCP task */
    EVAL_DHCP_SVC(ca_dhcp_service_net_ready();)

    ca_stdout_put_n("DHCP: starting DORA...");

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

/* Stats/pool/leak diagnostics: available on request via CLI "eth stats" |
 * "eth pool" | "eth dump" (caos/drivers/src/eth.c) — no periodic task here. */

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
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg))) {
        CA_SAFE_FAIL("ca_net_init");
    }

    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })

    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)

    /* NTP server: placeholder, updated by on_dhcp_bound with opt42 */
    EVAL_NTP_SVC(ca_ntp_service_init(NTP_SERVER_IP_FALLBACK, s_mac, on_ntp_sync, on_ntp_fail);)
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)
    EVAL_NTP_SVC(ca_ntp_service_start();)
    EVAL_UDP_SRV(ca_udp_server_start(&srv_cfg);)

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m36a, "=== Example 36 ===");
PRIVATE_STRING(m36b, "EN_ETH / EN_NET / EN_NET_DHCP / EN_DHCP_SERVICE / EN_UDP_SERVER / EN_NTP == 0: no ETH tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m36a);
    PUT_N(&priv_m36b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_NET_DHCP && EN_NET_NTP && ... */
