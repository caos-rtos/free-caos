// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 43 -- caOS Modbus TCP client
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically
 *   2. ARP service: server MAC resolution
 *   3. Modbus TCP client (port 502, unit_id 1):
 *        - FC03: reads 4 holding registers (addr 0..3)
 *        - FC06: writes a single register (addr 4)
 *        - FC16: writes 2 registers (addr 5..6)
 *        - FC01: reads 8 coils (addr 0..7)
 *        - cycle every MODBUS_POLL_PERIOD_MS
 *        - reconnects automatically if TCP drops
 *   4. ICMP echo reply (ping from the board)
 *   5. Diagnostics via CLI: "eth pool" | "modbus stat" | "tcp list"
 *
 * PC Server: start modbus_server.py in the example directory.
 *   sudo python3 modbus_server.py --iface enp7s0
 *   (starts simulated Modbus TCP server + DHCP, prints transactions)
 *
 * Flow:
 *   ca_os_configure():
 *     ca_modbus_service_init(server_ip, 502, 0.0.0.0, mac, unit_id=1)
 *
 *   on_dhcp_bound():
 *     update proto + arp + garp
 *     ca_sem_post(&s_dhcp_sem)   <- unlocks task_modbus
 *
 *   task_modbus:
 *     ca_sem_wait(&s_dhcp_sem)
 *     ca_modbus_service_net_ready()   <- starts TCP connect
 *     loop:
 *       if !connected -> retry every MODBUS_RECONNECT_MS
 *       every MODBUS_POLL_PERIOD_MS -> execute read/write cycle
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_ARP_SERVICE 1
 *   EN_DHCP_SERVICE 1, EN_NET_DHCP 1
 *   EN_TCP_SERVICE 1, EN_MODBUS_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#include "modbus_service.h"

#if EN_ETH && EN_NET && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_MODBUS_SERVICE

/* ============================================================================
 * Constants
 * ============================================================================ */
#define MODBUS_SERVER_IP      { 192U, 168U, 10U, 1U }
#define MODBUS_SERVER_PORT    502U
#define MODBUS_UNIT_ID        1U

#define MODBUS_POLL_PERIOD_MS   3000U   /* poll cycle every 3 s */
#define MODBUS_RECONNECT_MS     5000U   /* wait between reconnection attempts */

/* Demo register addresses */
#define REG_SENSOR_BASE   0U    /* FC03: read 4 registers from addr 0 */
#define REG_SETPOINT      4U    /* FC06: write setpoint to addr 4 */
#define REG_CONFIG_BASE   5U    /* FC16: write 2 registers from addr 5 */
#define COIL_STATUS_BASE  0U    /* FC01: read 8 coils from addr 0 */

/* ============================================================================
 * Hardware — NUCLEO-H753ZI, RMII
 * ============================================================================ */
#define LED_OK_PORT    CA_GPIO_PORT_E
#define LED_OK_PIN     1U    /* green  — transaction OK */
#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x43U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile ca_ipv4_t s_gw         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static ca_semaphore_t s_dhcp_sem;

static volatile u32 s_tx_ok  = 0U;
static volatile u32 s_tx_err = 0U;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x43U } },
    .link_cb  = on_link_change,
};

/* ============================================================================
 * DHCP UDP handler
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
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x43U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX43: Modbus TCP client ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);
    {
        ca_ipv4_t srv = { .v = MODBUS_SERVER_IP };
        ca_stdout_printf_n("Server  %u.%u.%u.%u:%u  unit_id=%u",
            (u32)srv.v[0], (u32)srv.v[1],
            (u32)srv.v[2], (u32)srv.v[3],
            (u32)MODBUS_SERVER_PORT, (u32)MODBUS_UNIT_ID);
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
 * Modbus task — read/write cycle
 * ============================================================================ */
static u32 task_modbus(void *arg)
{
    (void)arg;

    /* waits for DHCP bind */
    ca_sem_wait(&s_dhcp_sem);

    ca_ipv4_t server_ip = s_gw;   /* broker = DHCP gateway */
    ca_ipv4_t local_ip  = s_ip;

    ca_stdout_printf_n("MODBUS server %u.%u.%u.%u:%u",
        server_ip.v[0], server_ip.v[1],
        server_ip.v[2], server_ip.v[3],
        (u32)MODBUS_SERVER_PORT);

    ca_modbus_service_init(server_ip, MODBUS_SERVER_PORT,
                           local_ip, s_mac, MODBUS_UNIT_ID);

    ca_stdout_put_n("MODBUS: starting TCP connection...");
    ca_modbus_service_net_ready();

    u32 last_poll      = 0U;
    u32 last_reconnect = ca_os_tick_ms();   /* avoids immediate double init */
    u32 setpoint       = 100U;   /* demo value to write */

    while (1) {
        u32 now = ca_os_tick_ms();

        if (!ca_modbus_is_connected()) {
            if ((now - last_reconnect) >= MODBUS_RECONNECT_MS) {
                last_reconnect = now;
                ca_stdout_put_n("MODBUS: reconnect attempt...");
                local_ip  = s_ip;
                server_ip = s_gw;
                ca_modbus_service_init(server_ip, MODBUS_SERVER_PORT,
                                       local_ip, s_mac, MODBUS_UNIT_ID);
                ca_modbus_service_net_ready();
            }
            ca_os_sleep(500U);
            continue;
        }

        if ((now - last_poll) >= MODBUS_POLL_PERIOD_MS) {
            last_poll = now;
            bool ok   = true;

            /* FC03 — read 4 holding registers */
            u16 regs[4] = { 0U };
            if (ca_modbus_read_holding_regs(REG_SENSOR_BASE, 4U, regs)) {
                ca_stdout_printf_n(
                    "FC03 regs[0..3]= %u %u %u %u",
                    (u32)regs[0], (u32)regs[1],
                    (u32)regs[2], (u32)regs[3]);
            } else {
                ca_stdout_put_n("FC03 ERR");
                ok = false;
            }

            /* FC06 — write setpoint */
            if (ok) {
                if (ca_modbus_write_single_reg(REG_SETPOINT, (u16)setpoint)) {
                    ca_stdout_printf_n("FC06 setpoint=%u OK", setpoint);
                    setpoint = (setpoint >= 1000U) ? 100U : setpoint + 100U;
                } else {
                    ca_stdout_put_n("FC06 ERR");
                    ok = false;
                }
            }

            /* FC16 — write 2 config registers */
            if (ok) {
                u16 cfg_vals[2] = { 0x0001U, 0x0002U };
                if (ca_modbus_write_multiple_regs(REG_CONFIG_BASE,
                                                  cfg_vals, 2U)) {
                    ca_stdout_put_n("FC16 config OK");
                } else {
                    ca_stdout_put_n("FC16 ERR");
                    ok = false;
                }
            }

            /* FC01 — read 8 coils */
            if (ok) {
                u8 coils[1] = { 0U };
                if (ca_modbus_read_coils(COIL_STATUS_BASE, 8U, coils)) {
                    ca_stdout_printf_n("FC01 coils=0x%02X", (u32)coils[0]);
                } else {
                    ca_stdout_put_n("FC01 ERR");
                    ok = false;
                }
            }

            if (ok) {
                s_tx_ok++;
                ca_gpio_write(LED_OK_PORT, LED_OK_PIN, 1U);
                ca_os_sleep(50U);
                ca_gpio_write(LED_OK_PORT, LED_OK_PIN, 0U);
            } else {
                s_tx_err++;
            }
            ca_stdout_printf_n("cycle done  ok=%u err=%u",
                (u32)s_tx_ok, (u32)s_tx_err);
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
 * "eth pool" | "eth dump" | "modbus stat" - no periodic task here. */

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
    ca_gpio_configure(LED_OK_PORT, LED_OK_PIN,
        CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0U);
    ca_gpio_write(LED_OK_PORT, LED_OK_PIN, 0U);
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
    ca_os_job("modbus",  CA_TH_LEVEL3, task_modbus,  NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m43a, "=== Example 43: caOS Modbus TCP client ===");
PRIVATE_STRING(m43b, "DISABLED - requires EN_ETH / EN_NET / EN_ARP_SERVICE / EN_DHCP_SERVICE / EN_NET_DHCP / EN_TCP_SERVICE / EN_MODBUS_SERVICE");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m43a);
    PUT_N(&priv_m43b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_MODBUS_SERVICE */
