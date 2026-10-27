// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 80 -- caOS HTTP server with CLI via HTTP, over USB-NCM
 *
 * NCM variant of example_45 (same service on ETH). The application logic --
 * routes, handlers, HTML page, CLI capture -- is copied unchanged: what differs
 * is only the interface handed to ca_net_init(), thanks to the ca_net_iface_t
 * abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically
 *   2. HTTP server on port 80 (GET/POST/HEAD/OPTIONS):
 *        GET  /         -> HTML page with command form
 *        GET  /status   -> JSON with IP and uptime
 *        POST /cli      -> executes a CLI command, answers with its text output
 *        GET  /help     -> list of available CLI commands
 *   3. All CLI commands (top, free, tcp, arp, ...) reachable from a browser
 *   4. HTTPS on port 443 (EN_HTTPS_SERVICE): the same routes over TLS 1.3,
 *      identity from tools/gen_tls_identity.py (tls_identity.h, SAN caos.local)
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side -- DHCP server on the NCM interface, then a browser or curl:
 *   sudo python3 examples/example_80/http_test_ncm.py
 *   http://192.168.10.180/
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_TCP_SERVICE 1 | EN_ARP_SERVICE 1 |
 *                           EN_HTTP_SERVICE 1 | EN_CLI_CAPTURE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "http_service.h"
#if EN_HTTPS_SERVICE
#include "tls_identity.h"
#endif

/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
#define USB_NUM      0U

#elif MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U

#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS — single instance H563 */

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#endif


/* RAM budget — parts with 128 KB (F407, F446)
 *
 * The .heap must return to the end of RAM, so the data below it has to fit:
 * here it does not, and the link fails with "cannot move location counter
 * backwards" rather than with a clear message about memory.
 *
 * NET_BUF_COUNT is already 8 (16 KB) and TCP_MAX_CONN already 4 on these
 * parts; this example still overflows by ~4.7 KB. http_service alone holds
 * 6 KB of static buffers (4 KB tx + 2 KB rx), and underneath sit the two
 * per-EP USB queues in hw/f4xx/drivers/usb_fs_f4xx.c, 13 KB each.
 * Measured on both boards, byte for byte the same. */
#if MCU_ID == 407 || MCU_ID == 446
#define EX80_RAM_TOO_SMALL  1
#endif

#if EN_NET && EN_USB && EN_USB_NCM && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_HTTP_SERVICE && \
    !defined(EX80_RAM_TOO_SMALL)

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x80U } };

static volatile bool s_dhcp_bound = false;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xC0U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ── CLI output streaming over chunked HTTP ──────────────────────────────
 *
 * ex45 (the ETH original) captures the CLI into a fixed 2048B buffer with
 * ca_cli_exec_capture(): long output (config, top) is TRUNCATED -- the same
 * limit the SSH shell had before streaming landed on 17/08.
 *
 * Here the response is chunked (ca_http_resp_begin_chunked), so there is no
 * buffer to overflow: ca_cli_exec_capture_stream() hands each output chunk to
 * the sender below, which pushes it into the response as it is produced.
 *
 * The sender signature (u16 (*)(p_str)) carries no context argument, so the
 * target response travels through a static, exactly as ssh_transport.c does
 * with s_stream_sess. It is guarded by a mutex for the same reason: the stdout
 * drain thread can still be inside the sender after
 * ca_cli_exec_capture_stream() has returned (notably on timeout), and clearing
 * the pointer without the lock raced into a UsageFault there. */
static ca_http_response_t *s_stream_resp;
static ca_mutex_t             s_stream_lock;

static u16 http_stream_sender(p_str msg)
{
    if (!msg) return 0U;
    ca_mutex_lock(&s_stream_lock);
    ca_http_response_t *r = s_stream_resp;   /* re-read INSIDE the lock */
    if (r) ca_http_resp_write(r, msg->data, msg->size);
    ca_mutex_unlock(&s_stream_lock);
    return msg->size;
}

/* Runs one CLI command straight into a chunked response. */
static void cli_to_response(ca_http_response_t *resp, const char *cmd, u32 tmo)
{
    ca_mutex_lock(&s_stream_lock);
    s_stream_resp = resp;
    ca_mutex_unlock(&s_stream_lock);

    bool ok = ca_cli_exec_capture_stream(cmd, http_stream_sender, tmo);

    /* Clearing under the lock blocks until a sender still running on another
     * thread has finished -- never clear it unguarded. */
    ca_mutex_lock(&s_stream_lock);
    s_stream_resp = NULL;
    ca_mutex_unlock(&s_stream_lock);

    if (!ok) ca_http_resp_puts(resp, "\n(timeout)");
}

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
    "<h1>caOS Remote CLI (USB-NCM)</h1>"
    "<form method='POST' action='/cli'>"
    "<input type='text' name='cmd' placeholder='es: top, free, tcp, arp ...' autofocus>"
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
    ca_http_resp_puts(resp, ",\"link\":\"");
    ca_http_resp_puts(resp, ca_usb_ncm_is_up() ? "up" : "down");
    ca_http_resp_puts(resp, "\"}");
}

static void handler_help(const ca_http_request_t *req, ca_http_response_t *resp)
{
    (void)req;
    ca_http_resp_begin_chunked(resp, 200U, "text/plain");
    cli_to_response(resp, "help", 1000U);
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
    /* Chunked: output of any size, no 2048B ceiling. "config" (the longest
     * command, ~3KB with the SHA-256 hash as its last line) is the case that
     * used to truncate. */
    ca_http_resp_begin_chunked(resp, 200U, "text/plain");
    cli_to_response(resp, cmd, 1500U);
}

/* ── Route table ──────────────────────────────────────────────────────────── */

static const ca_http_route_t routes[] = {
    { "/",       HTTP_METHOD_GET | HTTP_METHOD_HEAD | HTTP_METHOD_OPTIONS, handler_root   },
    { "/status", HTTP_METHOD_GET | HTTP_METHOD_HEAD | HTTP_METHOD_OPTIONS, handler_status },
    { "/cli",    HTTP_METHOD_POST | HTTP_METHOD_OPTIONS,                   handler_cli    },
    { "/help",   HTTP_METHOD_GET | HTTP_METHOD_HEAD | HTTP_METHOD_OPTIONS, handler_help   },
};
#define ROUTE_COUNT  ((u8)(sizeof(routes) / sizeof(routes[0])))

/* ============================================================================
 * DHCP handler UDP (port 68)
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
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_arp_send_garp();
    ca_http_update_local_ip(ip);
    ca_os_sleep(10);

    /* Start (or restart) the listener with the leased address. Same shape as
     * ex45: ca_http_service_start() opens the listening PCB, which carries the
     * local IP, so it must run after the lease -- not before. */
    ca_http_service_start(routes, ROUTE_COUNT, 0U);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  GW=%u.%u.%u.%u",
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
    s_dhcp_bound = false;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },   /* 0.0.0.0 — updated after DHCP */
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x80U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX80: HTTP server + CLI via HTTP over USB-NCM ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("Port    TCP:%u  (HTTP)", (u32)HTTP_DEFAULT_PORT);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex45) */
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_printf_n("[USB] init FAILED"); return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_printf_n("[USB] add NCM FAILED - check EN_USB_NCM"); return 1U;
    }
    ca_usb_dev_mng_start();

    /* Attach NCM to the net core as push iface */
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_printf_n("[NET] ca_net_init FAILED"); return 1U;
    }

    /* Init stack with IP=0, MAC known */
    ca_net_proto_init(&net_cfg);

    /* Register port 68 to receive OFFER/ACK */
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    /* NB unlike ex45, ca_net_init() is here and not in ca_os_configure(): the
     * NCM iface only exists after USB enumeration. ca_http_service_start() is
     * called from on_dhcp_bound in both variants, so it needs no move.
     *
     * The DORA must not start before the host has brought the interface up:
     * the NCM link goes up only when the host selects alt setting 1, and until
     * then ca_usb_ncm_send_frame() drops every frame and returns false -- a
     * DISCOVER sent too early is LOST, not delayed. But the wait CANNOT sit
     * before the loop: enumeration needs this task to be already servicing
     * frames, so blocking here would hold off the very event being waited for.
     * The gate therefore lives INSIDE the loop.
     *
     * The HTTP server itself needs no gate: it only ever answers, and by the
     * time a browser can reach it the link is necessarily up. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            ca_dhcp_service_net_ready();
            ca_stdout_put_n("DHCP: starting DORA...");
        }

        /* Timed, not ca_sem_wait(): before the link is up no frame ever
         * arrives, so a plain wait would park here and the gate above would
         * never be re-evaluated. */
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) {
            ca_net_process();
        }
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex45
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            /* Coming back up with a lease already taken: ask for a renew so the
             * address is re-validated. That also restarts the HTTP listener,
             * since ca_http_service_start() sits inside on_dhcp_bound. */
            if (up && s_dhcp_bound) {
                ca_stdout_put_n("link back UP: requesting lease renew");
                ca_dhcp_service_renew();
            }
            prev = up;
        }
        ca_led_toggle(1U);
        ca_os_sleep(500);
    }
    return 0U;
}

/* ── Diagnostic CLI ──────────────────────────────────────────────────────── */
#if ENABLE_CLI
static bool dbg_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "usbdbg", 6) != 0) return false;
    EVAL_USB_DBG(ca_usb_dbg_print(USB_NUM);)
    return true;
}
static ca_cli_node_t s_dbg_cli = { dbg_cli_handler, NULL };
#endif

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
        ca_cli_register(&s_dbg_cli);
    )

    ca_mutex_init(&s_stream_lock);

    /* Heartbeat LED: configured here and not in task_ncm_mon because the pin
     * has a single writer but the rule from ex77 is to set up shared-context
     * GPIO before any task runs. */
    ca_led_init();
    ca_led_off(1U);

    /* ca_net_init() is NOT called here (unlike ex45): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */

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

    ca_os_job("net",     CA_TH_LEVEL4, task_net,     NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon", CA_TH_LEVEL3, task_ncm_mon, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m80a, "=== Example 80: HTTP server + CLI via HTTP over USB-NCM ===");
#ifdef EX80_RAM_TOO_SMALL
PRIVATE_STRING(m80b, "128K of RAM is not enough for http over NCM: short by ~4.7 KB");
#else
PRIVATE_STRING(m80b, "EN_NET / EN_USB / EN_USB_NCM / EN_TCP_SERVICE / EN_HTTP_SERVICE == 0: no tasks created");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m80a);
    PUT_N(&priv_m80b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_HTTP_SERVICE && ... */
