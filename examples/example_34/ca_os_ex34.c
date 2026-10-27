// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 34 -- caOS UDP server service on port 7961
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Functionality:
 *   1. UDP server listening on port 7961 via ca_udp_server_start()
 *   2. Each new client gets a dedicated worker thread (internal to the service)
 *   3. Echo of received data with prefix "ECHO: "
 *   4. Automatic disconnection after CLIENT_TIMEOUT_MS ms of inactivity
 *   5. Maximum MAX_CLIENTS simultaneous clients
 *
 * Test from PC:
 *   sudo ip addr add 192.168.10.1/24 dev eth0
 *   echo "hello" | nc -u 192.168.10.34 7961
 *   nc -u 192.168.10.34 7961      # interactive
 *
 * REQUIREMENTS os_config.h: EN_ETH 1, EN_NET 1, EN_ARP_SERVICE 1, EN_UDP_SERVER 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_UDP_SERVER

/* ============================================================================
 * Constants
 * ============================================================================ */
#define UDP_SRV_PORT       7961U
#define MAX_CLIENTS        4U
#define CLIENT_TIMEOUT_MS  10000U

/* ============================================================================
 * Hardware
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t  s_mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x34U } };
static const ca_ipv4_t s_ip  = { .v = { 192U, 168U, 10U, 34U } };

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x34U } },
    .link_cb  = on_link_change,
};

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

static void on_rx(u8 cid, ca_ipv4_t ip, u16 port,
                  const u8 *pld, u16 len)
{
    char txt[64U];
    u16 tlen = len < (u16)(sizeof(txt) - 1U) ? len : (u16)(sizeof(txt) - 1U);
    for (u16 i = 0U; i < tlen; i++) {
        u8 c = pld[i];
        txt[i] = (c >= 0x20U && c < 0x7FU) ? (char)c : '.';
    }
    txt[tlen] = '\0';
    ca_stdout_printf_n("UDP[7961] client %u: rx %uB ('%s') from %u.%u.%u.%u:%u",
        (u32)cid, (u32)len, txt, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);

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

static const ca_udp_server_cfg_t srv_cfg = {
    .port              = UDP_SRV_PORT,
    .max_clients       = MAX_CLIENTS,
    .client_timeout_ms = CLIENT_TIMEOUT_MS,
    .local_mac         = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x34U } },
    .local_ip          = { .v = { 192U, 168U, 10U, 34U } },
    .on_rx             = on_rx,
    .on_connect        = on_connect,
    .on_disconnect     = on_disconnect,
};

/* ============================================================================
 * Net config + net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 34U } },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x34U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_printf_n("\n\n=== EX34: UDP server port %u ===",
        (u32)UDP_SRV_PORT);
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("IP   %u.%u.%u.%u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3]);
    ca_os_sleep(100);
    ca_net_proto_init(&net_cfg);
    EVAL_UDP_SRV(ca_udp_server_bind();)
    ca_os_sleep(500);
    ca_eth_link_poll();
    ca_arp_send_garp();
    ca_os_sleep(50);

    ca_stdout_printf_n("UDP server ready");

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
    EVAL_ARP_SVC(ca_arp_service_init(s_mac, s_ip);)
}

void ca_os_main(void){
    EVAL_ARP_SVC(ca_arp_service_start();)
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
PRIVATE_STRING(m34a, "=== Example 34 ===");
PRIVATE_STRING(m34b, "EN_ETH /EN_NET / EN_UDP_SERVER == 0: no ETH tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m34a);
    PUT_N(&priv_m34b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_UDP_SERVER */
