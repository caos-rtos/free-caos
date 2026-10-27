// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 42 -- caOS MQTT client (publish status, receive LED commands)
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically
 *   2. ARP service: broker MAC resolution
 *   3. MQTT client (v3.1.1, QoS 0):
 *        - publishes to "caos/status" every MQTT_PUB_PERIOD_MS
 *          payload: {"seq":<n>,"up":<s>,"ip":"a.b.c.d"}
 *        - subscribes to "caos/led" → "on"/"off" → green LED
 *        - keepalive ping every MQTT_PING_PERIOD_MS (< MQTT_KEEPALIVE_SEC)
 *        - automatically reconnects after disconnection
 *   4. ICMP echo reply (ping from the board)
 *   5. Diagnostics via CLI: "eth pool" | "mqtt stat" | "tcp list"
 *
 * PC Server: start mqtt_server.py in the example directory.
 *   sudo python3 mqtt_server.py --iface enp7s0
 *   (starts mosquitto + DHCP, prints status, sends LED commands)
 *
 * Flow:
 *   ca_os_configure():
 *     ca_mqtt_service_init(broker_ip, 1883, 0.0.0.0, mac, "caos-h753",
 *                          on_mqtt_connected, on_mqtt_message, on_mqtt_disc)
 *
 *   on_dhcp_bound():
 *     updates proto + arp + garp
 *     ca_sem_post(&s_dhcp_sem)   ← unlocks task_mqtt
 *
 *   task_mqtt:
 *     ca_sem_wait(&s_dhcp_sem)
 *     ca_mqtt_service_net_ready()   ← starts TCP → MQTT CONNECT auto
 *     loop:
 *       if !connected → retry every MQTT_RECONNECT_MS
 *       every MQTT_PUB_PERIOD_MS → ca_mqtt_publish("caos/status", ...)
 *       every MQTT_PING_PERIOD_MS → ca_mqtt_ping()
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_ARP_SERVICE 1
 *   EN_DHCP_SERVICE 1, EN_NET_DHCP 1
 *   EN_TCP_SERVICE 1, EN_MQTT_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#include "mqtt_service.h"

#if EN_ETH && EN_NET && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_MQTT_SERVICE

/* ============================================================================
 * Constants
 * ============================================================================ */
#define MQTT_BROKER_IP      { 192U, 168U, 10U, 1U }
#define MQTT_BROKER_PORT    1883U
#define MQTT_CLIENT_ID      "caos-h753"
#define MQTT_TOPIC_STATUS   "caos/status"
#define MQTT_TOPIC_LED      "caos/led"

#define MQTT_PUB_PERIOD_MS   5000U   /* publish status every 5 s */
#define MQTT_PING_PERIOD_MS  30000U  /* keepalive ping every 30 s (< MQTT_KEEPALIVE_SEC=60) */
#define MQTT_RECONNECT_MS    8000U   /* delay between reconnection attempts */

/* ============================================================================
 * Hardware — NUCLEO-H753ZI, RMII
 * ============================================================================ */
#define LED_GREEN_PORT CA_GPIO_PORT_B
#define LED_GREEN_PIN  0U    /* green — controlled by MQTT */
#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x42U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile ca_ipv4_t s_gw         = { .addr = 0U };  /* gateway = broker IP */
static volatile bool   s_dhcp_bound = false;
static volatile u32    s_mqtt_conn  = 0U;   /* successful MQTT connection counter */

static ca_semaphore_t s_dhcp_sem;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x42U } },
    .link_cb  = on_link_change,
};

/* ============================================================================
 * MQTT callbacks (called from net task / TCP layer context)
 * ============================================================================ */
static void on_mqtt_connected(void)
{
    s_mqtt_conn++;
    ca_stdout_printf_n("MQTT CONNECTED  (#%u)", (u32)s_mqtt_conn);

    /* subscribe to LED command topic */
    ca_mqtt_subscribe(MQTT_TOPIC_LED);
    ca_stdout_put_n("MQTT subscribed -> " MQTT_TOPIC_LED);
}

static void on_mqtt_message(const char *topic, u16 topic_len,
                            const char *payload, u16 payload_len)
{
    /* use explicit lengths: topic and payload are NOT null-terminated */
    if (topic_len == (u16)(sizeof(MQTT_TOPIC_LED) - 1U) &&
        ca_memcmp(topic, MQTT_TOPIC_LED, topic_len) == 0)
    {
        if (payload_len >= 2U &&
            payload[0] == 'o' && payload[1] == 'n')
        {
            ca_gpio_write(LED_GREEN_PORT, LED_GREEN_PIN, 1U);
            ca_stdout_put_n("MQTT LED -> ON");
        } else {
            ca_gpio_write(LED_GREEN_PORT, LED_GREEN_PIN, 0U);
            ca_stdout_put_n("MQTT LED -> OFF");
        }
    }
}

static void on_mqtt_disconnected(void)
{
    ca_stdout_put_n("MQTT DISCONNECTED - reconnecting...");
}

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
    (void)mask; (void)dns;
    s_ip         = ip;
    s_gw         = gw;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3]);

    ca_arp_send_garp();
    ca_sem_post(&s_dhcp_sem);   /* unlock task_mqtt */
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
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x42U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX42: MQTT client ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);
    ca_stdout_printf_n("Broker  via DHCP GW :%u", (u32)MQTT_BROKER_PORT);
    ca_stdout_printf_n("Topics  pub=%s  sub=%s",
        MQTT_TOPIC_STATUS, MQTT_TOPIC_LED);

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
 * MQTT task
 * ============================================================================ */
static u32 task_mqtt(void *arg)
{
    (void)arg;

    static char payload[96];

    /* wait for DHCP bind */
    ca_sem_wait(&s_dhcp_sem);

    ca_ipv4_t broker_ip  = s_gw;   /* DHCP gateway = PC broker */
    ca_ipv4_t local_ip   = s_ip;

    ca_stdout_printf_n("MQTT broker %u.%u.%u.%u:%u",
        (u32)broker_ip.v[0], (u32)broker_ip.v[1],
        (u32)broker_ip.v[2], (u32)broker_ip.v[3],
        (u32)MQTT_BROKER_PORT);

    ca_mqtt_service_init(broker_ip, MQTT_BROKER_PORT,
                         local_ip, s_mac,
                         MQTT_CLIENT_ID,
                         on_mqtt_connected,
                         on_mqtt_message,
                         on_mqtt_disconnected);

    ca_stdout_put_n("MQTT: starting connection...");
    ca_mqtt_service_net_ready();

    u32 last_pub       = 0U;
    u32 last_ping      = 0U;
    u32 last_reconnect = ca_os_tick_ms();   /* avoids immediate double init */
    u32 pub_seq        = 0U;

    while (1) {
        u32 now = ca_os_tick_ms();

        if (!ca_mqtt_is_connected()) {
            if ((now - last_reconnect) >= MQTT_RECONNECT_MS) {
                last_reconnect = now;
                ca_stdout_put_n("MQTT: reconnect attempt...");
                /* update local IP in case of DHCP renewal */
                local_ip = s_ip;
                ca_mqtt_service_init(broker_ip, MQTT_BROKER_PORT,
                                     local_ip, s_mac,
                                     MQTT_CLIENT_ID,
                                     on_mqtt_connected,
                                     on_mqtt_message,
                                     on_mqtt_disconnected);
                ca_mqtt_service_net_ready();
            }
            ca_os_sleep(500U);
            continue;
        }

        /* publish status */
        if ((now - last_pub) >= MQTT_PUB_PERIOD_MS) {
            last_pub = now;
            ca_ipv4_t ip = s_ip;
            ca_snprintf(payload, sizeof(payload),
                "{\"seq\":%u,\"up\":%u,\"ip\":\"%u.%u.%u.%u\"}",
                pub_seq++,
                now / 1000U,
                (u32)ip.v[0], (u32)ip.v[1],
                (u32)ip.v[2], (u32)ip.v[3]);
            ca_mqtt_publish(MQTT_TOPIC_STATUS,
                            (const u8 *)payload,
                            (u16)ca_strlen(payload));
            ca_stdout_printf_n("MQTT pub -> %s", payload);
        }

        /* keepalive ping */
        if ((now - last_ping) >= MQTT_PING_PERIOD_MS) {
            last_ping = now;
            ca_mqtt_ping();
        }

        ca_os_sleep(200U);
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
 * "eth pool" | "eth dump" | "mqtt stat" - no periodic task here. */

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
    ca_os_job("mqtt",    CA_TH_LEVEL3, task_mqtt,    NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m42a, "=== Example 42: caOS MQTT client (publish status, receive LED commands) ===");
PRIVATE_STRING(m42b, "DISABLED - requires EN_ETH / EN_NET / EN_ARP_SERVICE / EN_DHCP_SERVICE / EN_NET_DHCP / EN_TCP_SERVICE / EN_MQTT_SERVICE");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m42a);
    PUT_N(&priv_m42b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_MQTT_SERVICE */
