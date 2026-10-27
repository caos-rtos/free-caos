// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 49 -- caOS TFTP client + DHCP
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Functionality:
 *   1. DHCP client — obtains IP automatically
 *   2. ARP service — cache + blocking resolve
 *   3. ICMP echo reply (ping)
 *   4. CLI — interactive commands
 *   5. TFTP client — GET and PUT via CLI:
 *        tftp get <ip> <filename>
 *        tftp put <ip> <filename> [size]   (buffer pattern in RAM, def 1024B)
 *        tftp info
 *   6. e49 dual <ip> — a fast PUT (32 KB) while a SLOW GET runs: the PC
 *        server (test_tftp_pc.py) waits 50 ms before each block of 'slow*'
 *        files. The PUT alone first, then started 20 ms after the slow GET:
 *          E49 DUAL: put alone X ms, put during a slow get Y ms, get Z ms
 *        Y near X: the transfers run side by side; Y near Z: the PUT waited
 *        for the slow GET to end.
 *
 * Test from PC (script in this example's directory, built-in DHCP server):
 *   sudo python3 examples/example_49/test_tftp_pc.py --iface <eth-iface> --put
 *   (--put is needed to accept "tftp put" uploads from the board)
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_NET_DHCP 1,
 *   EN_ARP_SERVICE 1, EN_DHCP_SERVICE 1,
 *   EN_TFTP_SERVICE 1, EN_CLI_TFTP 1, ENABLE_CLI 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_NET_DHCP && EN_DHCP_SERVICE && EN_TFTP_SERVICE

/* ============================================================================
 * Hardware NUCLEO-H753ZI
 * ============================================================================ */
#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U } };

static volatile ca_ipv4_t s_ip        = { .addr = 0U };
static volatile bool   s_dhcp_done = false;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U } },
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
    (void)dns;
    s_ip        = ip;
    s_dhcp_done = true;

    ca_net_proto_set_local_ip(ip);
#if EN_ARP_SERVICE
    ca_arp_service_update_local_ip(ip);
#endif

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  mask=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3],
        mask.v[0], mask.v[1], mask.v[2], mask.v[3]);
    ca_stdout_printf_n("            GW=%u.%u.%u.%u  DNS=%u.%u.%u.%u",
        gw.v[0], gw.v[1], gw.v[2], gw.v[3],
        dns.v[0], dns.v[1], dns.v[2], dns.v[3]);
#if EN_ARP_SERVICE
    ca_arp_send_garp();
#endif
}

static void on_dhcp_expire(void)
{
    s_dhcp_done = false;
    s_ip.addr   = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX49: DHCP + TFTP client ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);

    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
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
 * "e49 dual <ip>": a PUT and a GET together, through the public API only
 * ============================================================================ */
#define E49_DUAL_PUT_BYTES  32768U

typedef struct {
    ca_semaphore_t go;
    bool           ok;
    u32            ms;        /* duration of the call */
    u32            bytes;
} dual_job_t;

static u8             s_up[E49_DUAL_PUT_BYTES];
static ca_ipv4_t      s_dual_ip;
static dual_job_t     s_put_job, s_get_job;
static ca_semaphore_t s_dual_done;            /* posted by each worker */
static ca_semaphore_t s_dual_go;              /* posted by the CLI */
static volatile bool  s_dual_busy = false;

static ca_tftp_cfg_t dual_cfg(const char *file)
{
    ca_tftp_cfg_t c = {
        .server_ip = s_dual_ip, .filename = file,
        .local_mac = s_mac, .local_ip = s_ip,
    };
    return c;
}

static bool dual_on_block(u16 blk, const u8 *data, u16 len, void *arg)
{
    (void)blk; (void)data;
    *(u32 *)arg += len;
    return true;
}

static u32 task_dual_put(void *arg)
{
    (void)arg;
    while (1) {
        ca_sem_wait(&s_put_job.go);
        ca_tftp_cfg_t c = dual_cfg("dual_up.bin");
        u32 t0 = ca_os_tick_ms();
        s_put_job.ok    = ca_tftp_put(&c, s_up, E49_DUAL_PUT_BYTES);
        s_put_job.ms    = ca_os_tick_ms() - t0;
        s_put_job.bytes = E49_DUAL_PUT_BYTES;
        ca_sem_post(&s_dual_done);
    }
    return 0U;
}

static u32 task_dual_get(void *arg)
{
    (void)arg;
    while (1) {
        ca_sem_wait(&s_get_job.go);
        ca_tftp_cfg_t c = dual_cfg("slow.bin");
        s_get_job.bytes = 0U;
        u32 t0 = ca_os_tick_ms();
        s_get_job.ok = ca_tftp_get(&c, dual_on_block, (void *)&s_get_job.bytes);
        s_get_job.ms = ca_os_tick_ms() - t0;
        ca_sem_post(&s_dual_done);
    }
    return 0U;
}

/* Starts the jobs given, waits for all of them */
static void dual_run(bool put, bool get)
{
    if (put) ca_sem_post(&s_put_job.go);
    if (get) ca_sem_post(&s_get_job.go);
    for (u32 n = (put ? 1U : 0U) + (get ? 1U : 0U); n > 0U; n--)
        ca_sem_wait(&s_dual_done);
}

static u32 task_dual(void *arg)
{
    (void)arg;
    for (u32 i = 0U; i < E49_DUAL_PUT_BYTES; i++) s_up[i] = (u8)(i & 0xFFU);
    while (1) {
        ca_sem_wait(&s_dual_go);
        dual_run(true, false);                       /* the PUT alone */
        const u32 put_alone = s_put_job.ms; const bool ok1 = s_put_job.ok;

        ca_sem_post(&s_get_job.go);                  /* the slow GET first... */
        ca_os_sleep(20);
        ca_sem_post(&s_put_job.go);                  /* ...then the PUT */
        ca_sem_wait(&s_dual_done);
        ca_sem_wait(&s_dual_done);

        ca_stdout_printf_n("E49 DUAL: put alone %u ms, put during a slow get %u ms, "
                           "get %u B %u ms %s",
                           put_alone, s_put_job.ms, s_get_job.bytes, s_get_job.ms,
                           (ok1 && s_put_job.ok && s_get_job.ok) ? "ALL OK" : "FAIL");
        s_dual_busy = false;
    }
    return 0U;
}

static bool ex49_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e49       : dual <ip>  (PUT 32 KB alone, then during a slow GET)");
        return false;
    }
    if (ca_strncmp(p, "e49", 3) != 0) return false;
    p = ca_skip_sp(p + 3);
    if (ca_strncmp(p, "dual", 4) != 0) { CLI_PUT_N("e49: dual <ip>"); return true; }
    p = ca_skip_sp(p + 4);
    ca_ipv4_t ip;
    if (!ca_parse_ipv4(p, &ip)) { CLI_PUT_N("e49 dual: bad ip"); return true; }
    if (s_dual_busy) { CLI_PUT_N("e49 dual: already running"); return true; }
    s_dual_busy = true;
    s_dual_ip   = ip;
    ca_sem_post(&s_dual_go);
    return true;
}

static ca_cli_node_t s_ex49_cli_node = { ex49_cli_handler, NULL };

/* ============================================================================
 * Heartbeat + ETH monitor
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

/* ============================================================================
 * GPIO init
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
        ca_cli_register(&s_ex49_cli_node);
    )
    ca_sem_init(&s_dual_go,      "E49GO", 0U, 1U);
    ca_sem_init(&s_dual_done,    "E49DN", 0U, 2U);
    ca_sem_init(&s_put_job.go,   "E49PT", 0U, 1U);
    ca_sem_init(&s_get_job.go,   "E49GT", 0U, 1U);
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    
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
    EVAL_TFTP_SVC(ca_tftp_service_init(s_mac);)
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("dual",      CA_TH_LEVEL3, task_dual,      NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("dual_put",  CA_TH_LEVEL3, task_dual_put,  NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("dual_get",  CA_TH_LEVEL3, task_dual_get,  NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m49a, "=== Example 49: caOS TFTP client + DHCP ===");
PRIVATE_STRING(m49b, "DISABLED - requires EN_ETH / EN_NET / EN_NET_DHCP / EN_DHCP_SERVICE / EN_TFTP_SERVICE");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m49a);
    PUT_N(&priv_m49b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_NET_DHCP && EN_DHCP_SERVICE && EN_TFTP_SERVICE */
