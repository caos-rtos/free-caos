// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 48 -- caOS IP defrag + DHCP + UDP server
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Features:
 *   1. DHCP client — gets IP automatically
 *   2. ARP service — cache + blocking resolve
 *   3. ICMP echo reply (ping)
 *   4. IP defrag service — reassembles fragmented packets (EN_IP_DEFRAG=1)
 *   5. UDP server on port 7961 — receives payload up to 2048 bytes
 *
 * For every datagram received (reassembled ones included) the server logs
 * length and the hardware CRC32 of the payload, then echoes the payload back,
 * so the test PC can verify the reassembly is byte-exact.
 *
 * Test from PC (script in this example's directory, needs scapy):
 *   sudo python3 examples/example_48/test_frag_pc.py --iface <eth-iface>
 *   (built-in mini DHCP server: no dnsmasq needed)
 *
 * os_config.h REQUIREMENTS:
 *   EN_ETH 1, EN_NET 1, EN_NET_DHCP 1,
 *   EN_ARP_SERVICE 1, EN_UDP_SERVER 1, EN_DHCP_SERVICE 1,
 *   EN_IP_DEFRAG 1, EN_CRC 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_NET_DHCP && EN_DHCP_SERVICE && EN_UDP_SERVER && EN_IP_DEFRAG

/* ============================================================================
 * Constants
 * ============================================================================ */
#define UDP_SRV_PORT        7961U
#define MAX_CLIENTS         2U
#define CLIENT_TIMEOUT_MS   30000U
/* IP_DEFRAG_MAX_PAYLOAD defined in os_config.h (default 1480) */

/* ============================================================================
 * NUCLEO-H753ZI Hardware
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U } };

static volatile ca_ipv4_t s_ip   = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U } },
    .link_cb  = on_link_change,
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
    s_ip         = ip;
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
    (void)dns;
    ca_arp_send_garp();
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * UDP server callbacks
 * ============================================================================ */
static void on_connect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] cid=%u  %u.%u.%u.%u:%u connected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_disconnect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] cid=%u  %u.%u.%u.%u:%u disconnected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_rx(u8 cid, ca_ipv4_t ip, u16 port, const u8 *pld, u16 len)
{
    /* Hardware CRC32: unlike a byte sum it also catches reordered or swapped
     * bytes, which is exactly what has to be proven when reassembling IP
     * fragments. Runs in the UDP worker task (never in ISR), so the mutex
     * inside ca_crc32_get() is safe here. */
    u32 csum = ca_crc32_get((u8 *)pld, (u32)len);

    ca_stdout_printf_n("UDP[7961] cid=%u  rx=%u B  crc32=0x%08X  from %u.%u.%u.%u:%u",
        (u32)cid, (u32)len, csum,
        ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);

    if (len >= 8U) {
        ca_stdout_printf_n("  pld[0..7]= %02X %02X %02X %02X %02X %02X %02X %02X",
            pld[0], pld[1], pld[2], pld[3],
            pld[4], pld[5], pld[6], pld[7]);
    }

    /* Full payload echo — analyzable with tcpdump for byte-by-byte verification */
    ca_udp_server_send(cid, pld, len);
}

static ca_udp_server_cfg_t srv_cfg = {
    .port              = UDP_SRV_PORT,
    .max_clients       = MAX_CLIENTS,
    .client_timeout_ms = CLIENT_TIMEOUT_MS,
    .local_mac         = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U } },
    .local_ip          = { .addr = 0U },
    .on_rx             = on_rx,
    .on_connect        = on_connect,
    .on_disconnect     = on_disconnect,
};

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x48U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX48: DHCP + UDP server + IP defrag ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],
        s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("defrag: sessions=%u timeout=%ums payload_max=%uB",
        (u32)IP_DEFRAG_MAX_SESSIONS,
        (u32)IP_DEFRAG_TIMEOUT_MS,
        (u32)IP_DEFRAG_MAX_PAYLOAD);

    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    EVAL_UDP_SRV(ca_udp_server_bind();)
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

/* ============================================================================
 * Stats task
 * ============================================================================ */
/* Stats/pool/leak diagnostics: available on request via CLI "eth stats" |
 * "eth pool" | "eth dump" | "udp stats" - no periodic task here. */

/* ============================================================================
 * Peripherals / GPIO
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

    /* Hardware CRC32 used by on_rx() to verify reassembled payloads */
    EVAL_CRC(ca_crc32_init();)

    
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");

    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })
    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)

    /* IP defrag: allocates DEFRAG_MAX_PAYLOAD × IP_DEFRAG_MAX_SESSIONS from heap */
    EVAL_IP_DEFRAG(
        if (!ca_ip_defrag_init(IP_DEFRAG_MAX_PAYLOAD))
            CA_SAFE_FAIL("ca_ip_defrag_init");
    )
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)
    EVAL_UDP_SRV(ca_udp_server_start(&srv_cfg);)
    EVAL_IP_DEFRAG(ca_ip_defrag_service_start();)
    EVAL_PACKET_TRACER(
        ca_pktrace_set_sender(STDOUT);
        ca_pktrace_start();
        ca_pktrace_enable(true);
    )

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
PRIVATE_STRING(m48a, "=== Example 48: caOS IP defrag + DHCP + UDP server ===");
PRIVATE_STRING(m48b, "DISABLED - requires EN_ETH / EN_NET / EN_NET_DHCP / EN_DHCP_SERVICE / EN_UDP_SERVER / EN_IP_DEFRAG");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m48a);
    PUT_N(&priv_m48b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_NET_DHCP && EN_DHCP_SERVICE && EN_UDP_SERVER && EN_IP_DEFRAG */
