// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 45 -- caOS HTTP server with CLI via HTTP
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Features:
 *   1. DHCP client: obtains IP automatically
 *   2. HTTP server on port 80 (GET/POST/HEAD/OPTIONS):
 *        GET  /         → HTML page with command form
 *        GET  /status   → JSON with IP, uptime, network stats
 *        POST /cli      → executes CLI command, responds with text output
 *        GET  /help     → list of available CLI commands
 *   3. All CLI commands (top, free, tcp, eth, arp, ...) accessible from browser
 *   4. HTTPS on port 443 (EN_HTTPS_SERVICE): the same routes over TLS 1.3,
 *      identity from tools/gen_tls_identity.py (tls_identity.h, SAN caos.local)
 *
 * Test from PC:
 *   Browser: http://<board_ip>/
 *   curl:    curl http://<board_ip>/status
 *            curl -X POST http://<board_ip>/cli -d "cmd=top"
 *            curl -X POST http://<board_ip>/cli -d "cmd=free"
 *   HTTPS:   curl --cacert examples/example_45/tls_identity_cert.pem \
 *                 --resolve caos.local:443:<board_ip> https://caos.local/status
 *
 * ca_os_configure(): init hardware, driver, network services
 * ca_os_main()     : task startup
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_ARP_SERVICE 1
 *   EN_DHCP_SERVICE 1, EN_NET_DHCP 1
 *   EN_TCP_SERVICE 1, EN_HTTP_SERVICE 1, EN_CLI_CAPTURE 1
 *   EN_PACKET_TRACER 1 (optional — CLI: trace on/off/filter)
 */

#include "caos.h"
#include "net_proto.h"
#include "http_service.h"
#if EN_HTTPS_SERVICE
#include "tls_identity.h"
#endif

#if EN_ETH && EN_NET && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_HTTP_SERVICE

/* ── Config ───────────────────────────────────────────────────────────────── */

#define ETH_AF         11U

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x01U, 0x02U, 0x45U } },
};

static ca_mac_t s_mac;

/* CLI capture buffer — shared between HTTP handlers */
#define CLI_CAP_SIZE  2048U
static char g_cli_cap[CLI_CAP_SIZE];

/* ── Root HTML page (flash) ─────────────────────────────────────────────── */

static const char PAGE_ROOT[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<title>caOS CLI</title>"
    "<style>"
    "body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;margin:20px}"
    "h1{color:#569cd6}input{width:400px;background:#252526;color:#d4d4d4;"
    "border:1px solid #555;padding:4px}button{background:#0e639c;color:#fff;"
    "border:none;padding:5px 12px;cursor:pointer}"
    "pre{background:#252526;padding:10px;white-space:pre-wrap;min-height:80px}"
    "</style></head><body>"
    "<h1>caOS Remote CLI</h1>"
    "<form method='POST' action='/cli'>"
    "<input type='text' name='cmd' placeholder='es: top, free, eth, arp ...' autofocus>"
    "&nbsp;<button type='submit'>Run</button>"
    "</form>"
    "<p><a href='/status'>JSON status</a> &nbsp; <a href='/help'>help</a></p>"
    "</body></html>";

/* ── HTTP handlers ────────────────────────────────────────────────────────── */

static void handler_root(const ca_http_request_t *req, ca_http_response_t *resp)
{
    (void)req;
    ca_http_resp_begin(resp, 200U, "text/html");
    ca_http_resp_write(resp, PAGE_ROOT, (u16)(sizeof(PAGE_ROOT) - 1U));
}

static void handler_status(const ca_http_request_t *req, ca_http_response_t *resp)
{
    (void)req;
    ca_ipv4_t ip = ca_dhcp_service_get_ip();
    u64 ms    = ca_os_tick_64_ms();
    u32 secs  = (u32)(ms / 1000ULL);

    ca_http_resp_begin(resp, 200U, "application/json");
    ca_http_resp_puts(resp, "{\"ip\":\"");
    ca_http_resp_puti(resp, ip.v[0]);
    ca_http_resp_puts(resp, ".");
    ca_http_resp_puti(resp, ip.v[1]);
    ca_http_resp_puts(resp, ".");
    ca_http_resp_puti(resp, ip.v[2]);
    ca_http_resp_puts(resp, ".");
    ca_http_resp_puti(resp, ip.v[3]);
    ca_http_resp_puts(resp, "\",\"uptime_s\":");
    ca_http_resp_puti(resp, secs);
    ca_http_resp_puts(resp, "}");
}

static void handler_help(const ca_http_request_t *req, ca_http_response_t *resp)
{
    (void)req;
    ca_http_resp_begin(resp, 200U, "text/plain");
    if (!ca_cli_exec_capture("help", g_cli_cap, CLI_CAP_SIZE, 1000U))
        ca_http_resp_puts(resp, "timeout");
    else
        ca_http_resp_puts(resp, g_cli_cap);
}

/* Extracts "cmd=..." from POST body (URL-encoded form) */
static u16 extract_cmd(const u8 *body, u16 body_len, char *out, u16 out_size)
{
    if (!body || body_len == 0U) return 0U;
    const char *p   = (const char *)body;
    const char *end = p + body_len;
    bool found = false;
    while (p + 4U <= end) {
        if (p[0]=='c' && p[1]=='m' && p[2]=='d' && p[3]=='=') {
            p += 4U; found = true; break;
        }
        p++;
    }
    if (!found || p >= end) return 0U;
    u16 i = 0U;
    while (p < end && *p != '&' && i < (u16)(out_size - 1U)) {
        char c = *p++;
        if (c == '+') c = ' ';
        if (c == '%' && p + 1U < end) {
            u8 hi = (u8)(*p   >= 'a' ? *p   - 'a' + 10 : *p   >= 'A' ? *p   - 'A' + 10 : *p   - '0');
            u8 lo = (u8)(*(p+1)>= 'a' ? *(p+1)-'a' + 10 : *(p+1)>= 'A' ? *(p+1)-'A' + 10 : *(p+1)-'0');
            c = (char)((hi << 4) | lo);
            p += 2U;
        }
        out[i++] = c;
    }
    out[i] = '\0';
    return i;
}

static void handler_cli(const ca_http_request_t *req, ca_http_response_t *resp)
{
    char cmd[128];
    u16 clen = extract_cmd(req->body, req->body_len, cmd, sizeof(cmd));

    if (clen == 0U) {
        ca_http_resp_bad_request(resp);
        return;
    }
    ca_http_resp_begin(resp, 200U, "text/plain");
    if (!ca_cli_exec_capture(cmd, g_cli_cap, CLI_CAP_SIZE, 1500U)) {
        ca_http_resp_puts(resp, "timeout");
    } else if (g_cli_cap[0] == '\0') {
        ca_http_resp_puts(resp, "(no output)");
    } else {
        ca_http_resp_puts(resp, g_cli_cap);
    }
}

/* ── Route table ──────────────────────────────────────────────────────────── */

static const ca_http_route_t routes[] = {
    { "/",       HTTP_METHOD_GET | HTTP_METHOD_HEAD | HTTP_METHOD_OPTIONS, handler_root   },
    { "/status", HTTP_METHOD_GET | HTTP_METHOD_HEAD | HTTP_METHOD_OPTIONS, handler_status },
    { "/cli",    HTTP_METHOD_POST | HTTP_METHOD_OPTIONS,                   handler_cli    },
    { "/help",   HTTP_METHOD_GET | HTTP_METHOD_HEAD | HTTP_METHOD_OPTIONS, handler_help   },
};
#define ROUTE_COUNT  ((u8)(sizeof(routes) / sizeof(routes[0])))

/* ── DHCP ─────────────────────────────────────────────────────────────────── */

static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b   = ca_net_buf_ptr(buf_idx);
    u8           *frm = b->data + b->data_off;
    u16           l4  = b->meta.rx.l4_off;
    const ca_udp_frame_t *udp = (const ca_udp_frame_t *)(frm + l4);
    ca_dhcp_service_rx_handler(udp, (u16)(b->data_len - l4));
    ca_net_buf_free(buf_idx);
}

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    (void)mask; (void)gw; (void)dns;
    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_arp_send_garp();
    ca_http_update_local_ip(ip);
    ca_os_sleep(10);
    ca_http_service_start(routes, ROUTE_COUNT, 0U);
    ca_stdout_printf_n("DHCP BOUND  IP=%u.%u.%u.%u  GW=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3],
        gw.v[0], gw.v[1], gw.v[2], gw.v[3]);
    ca_stdout_printf_n("HTTP  http://%u.%u.%u.%u/",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    EVAL_HTTPS_SVC(
        if (ca_https_service_start(TLS_IDENTITY_CERT, sizeof(TLS_IDENTITY_CERT),
                                   TLS_IDENTITY_KEY, 0U)) {
            ca_stdout_printf_n("HTTPS https://%u.%u.%u.%u/",
                ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
        } else {
            ca_stdout_put_n("HTTPS not started (key or keys_store)");
        }
    )
}

static void on_dhcp_expire(void)
{
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ── ETH monitor ──────────────────────────────────────────────────────────── */

static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

/* ── Net config ───────────────────────────────────────────────────────────── */

static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x01U, 0x02U, 0x45U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

/* ── Tasks ────────────────────────────────────────────────────────────────── */

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX45: HTTP server + CLI via HTTP ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);

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

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) { ca_eth_link_poll(); ca_os_sleep(500); }
    return 0U;
}

static u32 task_hb(void *arg)
{
    (void)arg;
    while (1) {
        ca_led_toggle(2U);
        ca_os_sleep(500);
    }
    return 0U;
}

/* ── GPIO ETH — NUCLEO-H753ZI (RMII) ─────────────────────────────────────── */

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

/* ── caOS hooks ───────────────────────────────────────────────────────────── */

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
    ca_led_off(2U);

    eth_gpio_init();
    eth_cfg.link_cb = on_link_change;
    s_mac = eth_cfg.mac;

    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");

    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    }
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();
    EVAL_PACKET_TRACER(
        ca_pktrace_set_sender(STDOUT);
        ca_pktrace_start();
    )

    ca_os_job("net",     CA_TH_LEVEL6, task_net,     NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("hb",      CA_TH_LEVEL4, task_hb,      NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m45a, "=== Example 45: caOS HTTP server with CLI via HTTP ===");
PRIVATE_STRING(m45b, "DISABLED - requires EN_ETH / EN_NET / EN_ARP_SERVICE / EN_DHCP_SERVICE / EN_NET_DHCP / EN_TCP_SERVICE / EN_HTTP_SERVICE");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m45a);
    PUT_N(&priv_m45b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_HTTP_SERVICE */
