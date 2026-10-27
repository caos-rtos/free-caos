// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 85 -- several TCP connections at once: how the buffer pool is shared
 *
 * Hardware: NUCLEO-H753ZI / NUCLEO-H563ZI / NUCLEO-F767ZI (RMII, LAN8742A)
 *
 * WHY THIS EXAMPLE EXISTS
 *
 * ex44 measures one connection, and one connection cannot show what this has
 * to show. Every TCP connection's receive ring holds buffers out of ONE pool
 * (NET_BUF_COUNT), so on paper four connections at full ring depth ask for
 * 4 x (16 + 16) + 8 = 136 buffers where 64 exist. The static answer is to make
 * the rings smaller, and that was measured on ex44/H563, 2026-09-16:
 *
 *     16/16    RX 35.9 Mbps    TX 31.8 Mbps
 *      8/4     RX 25.9 Mbps    TX  4.5 Mbps
 *
 * Both directions pay and the transmit side collapses, so the depth stays at
 * its maximum and the sharing moves to runtime: rx_quota() in tcp_service.c
 * gives each connection a share of what the pool can spare, recomputed as
 * connections come and go. With one connection that share is the whole ring
 * and nothing changes - which ex44 confirms, 35.1 Mbps against 35.9 static.
 *
 * WHAT THIS EXAMPLE ANSWERS
 *
 * Whether that quota holds up when connections really compete:
 *
 *   sum stays near one connection's rate  -> the sharing works. The limit is
 *                                            the per-packet cost of the stack,
 *                                            which is shared whatever the
 *                                            number of connections
 *   sum collapses                         -> the sharing costs more than it
 *                                            saves
 *   one stream stalls at zero             -> a connection starved the others,
 *                                            which is what the quota exists to
 *                                            prevent
 *   a connection is refused               -> the pool ran out before the quota
 *                                            could limit anyone: the reserve
 *                                            (TCP_RX_POOL_RESERVE) is too small
 *
 * MEASURED, H563, 4 streams of 10 s (2026-09-16): 4 x 23.7 Mbps, 95.0 Mbps in
 * TOTAL, the four within 0.2% of each other.
 *
 * That is not four times a share of 36 - it is nearly three times what ONE
 * connection reaches, and it says the single-connection figure was never a
 * measure of how much work the board can do. 95.0 Mbps of payload is 100.1 on
 * the wire once headers, preamble and interframe gap are counted: the link is
 * saturated, and the bottleneck has moved off the board entirely.
 *
 * What limited one connection was the WINDOW TURNAROUND - the time spent
 * waiting for an ACK before more can arrive - and four connections fill each
 * other's gaps. In events per second the stack went from ~6,100 to ~16,300,
 * so the per-packet cost was not the ceiling either.
 *
 * The equal split across the four is the result that matters here: it is the
 * quota doing its job. Note that this measurement can no longer see the
 * board's own limit, because the cable runs out first.
 *
 * CALLBACK MODE, NOT ONE TASK PER CONNECTION
 *
 * ca_tcp_listen() with on_accept/on_rx set runs in the net task's context, so
 * N connections cost no extra stacks - which matters, because the point is to
 * measure the buffer pool and not to spend the RAM under test on task stacks.
 * It is also why ex44's iperf service cannot be used here: it does ONE
 * blocking ca_tcp_accept() and then a blocking receive loop, so it is
 * single-connection by construction.
 *
 * Peer PC:
 *   sudo python3 examples/example_85/msink_peer.py \
 *        --parallel 4 --board-ip <ip> --dur 10
 *
 * The board STARTS LISTENING BY ITSELF two seconds after it gets its address,
 * so a run needs nothing typed on its console: the peer has to be up first
 * anyway (it is what serves DHCP), and by then it is already retrying its
 * connections. The commands are there for a second run or another port:
 *
 * Board console:
 *   msink start [port]    accept connections and count bytes
 *   msink stats           per-connection totals and the sum
 *   msink stop            close everything and report
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_TCP_SERVICE 1, EN_ARP_SERVICE 1, EN_NET_DHCP 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_TCP_SERVICE && EN_ARP_SERVICE

/* ============================================================================
 * Hardware — NUCLEO, RMII
 * ============================================================================ */
#define ETH_AF         11U

#define SINK_PORT_DEFAULT   5001U
/* Delay between the lease arriving and the listener opening by itself. Two
 * seconds: long enough for the gratuitous ARP to have gone out and the peer to
 * have learnt the address, short enough that the peer's connection retries
 * (60 s) are nowhere near running out. */
#define SINK_AUTOSTART_MS   2000U
/* One slot per connection the TCP service can hold. Sized from TCP_MAX_CONN
 * rather than from the four the peer opens: the whole point is to see what
 * happens when more arrive than the pool can serve at full depth. */
#define SINK_MAX_CONN       TCP_MAX_CONN

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x85U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x85U } },
    .link_cb  = on_link_change,
};

/* ============================================================================
 * Multi-connection sink
 *
 * One entry per accepted connection. Written from the net task (on_rx) and
 * read from the CLI task, so the counters are volatile and each one is only
 * ever written by ONE context - no lock is needed for a u32 the reader may
 * see a few frames stale, and taking one in on_rx would put the CLI on the
 * receive path.
 * ========================================================================== */
typedef struct {
    volatile u8   handle;      /* 0 = free slot                              */
    volatile u32  bytes;       /* payload counted on this connection         */
    volatile u32  segs;        /* on_rx calls: bytes/segs is the mean segment */
    volatile u32  t_first;     /* tick of the first byte                     */
    volatile u32  t_last;      /* tick of the most recent byte              */
} sink_conn_t;

static sink_conn_t s_conn[SINK_MAX_CONN];
static volatile u8   s_listen_h;      /* listen handle, 0 = not listening   */
static volatile u16  s_port = SINK_PORT_DEFAULT;
static volatile u16  s_accepted;      /* connections accepted since start   */
static volatile u16  s_refused;       /* accepted with no free slot left    */

static sink_conn_t *conn_find(u8 h)
{
    for (u8 i = 0U; i < SINK_MAX_CONN; i++)
        if (s_conn[i].handle == h) return &s_conn[i];
    return NULL;
}

static void sink_on_accept(u8 h)
{
    sink_conn_t *c = conn_find(0U);
    if (!c) {
        /* Nowhere to record it. The connection is live either way - the TCP
         * service accepted it - so closing it here is what keeps the board's
         * view and the peer's view the same. */
        s_refused++;
        ca_tcp_close(h);
        return;
    }
    c->bytes   = 0U;
    c->segs    = 0U;
    c->t_first = 0U;
    c->t_last  = 0U;
    c->handle  = h;              /* last: the CLI may read while this runs */
    s_accepted++;
}

static void sink_on_rx(u8 h, const u8 *data, u16 len)
{
    (void)data;                  /* a sink: the bytes are counted, not kept */
    sink_conn_t *c = conn_find(h);
    if (!c) return;

    u32 now = ca_os_tick_ms();
    if (c->bytes == 0U) c->t_first = now;
    c->bytes += (u32)len;
    c->segs++;
    c->t_last = now;
}

static void sink_on_closed(u8 h)
{
    sink_conn_t *c = conn_find(h);
    /* The totals are deliberately NOT cleared: a run is read after the peer
     * has finished, and by then every connection has closed. Only the handle
     * goes, which frees the slot. */
    if (c) c->handle = 0U;
}

static bool sink_start(u16 port)
{
    if (s_listen_h != 0U) return false;

    for (u8 i = 0U; i < SINK_MAX_CONN; i++) {
        s_conn[i].handle = 0U;
        s_conn[i].bytes  = 0U;
        s_conn[i].segs   = 0U;
    }
    s_accepted = 0U;
    s_refused  = 0U;

    ca_tcp_listen_cfg_t lcfg = {
        .local_port = port,
        .local_ip   = s_ip,
        .local_mac  = s_mac,
        .on_accept  = sink_on_accept,
        .on_rx      = sink_on_rx,
        .on_closed  = sink_on_closed,
    };
    s_listen_h = ca_tcp_listen(&lcfg);
    s_port     = port;
    return (s_listen_h != 0U);
}

static void sink_stop(void)
{
    if (s_listen_h != 0U) {
        ca_tcp_unlisten(s_listen_h);
        s_listen_h = 0U;
    }
    for (u8 i = 0U; i < SINK_MAX_CONN; i++) {
        u8 h = s_conn[i].handle;
        if (h != 0U) ca_tcp_close(h);
    }
}

/* Mbps x1000, in integer arithmetic: bytes*8 would overflow a u32 at 512 MB,
 * so the division comes first and the result is kept in kbps. */
static u32 kbps_of(u32 bytes, u32 ms)
{
    if (ms == 0U) return 0U;
    return (u32)(((u64)bytes * 8ULL) / (u64)ms);
}

static void sink_report(void)
{
    u32 total = 0U, segs = 0U;
    u32 first = 0xFFFFFFFFU, last = 0U;
    u8  live  = 0U, used = 0U;

    for (u8 i = 0U; i < SINK_MAX_CONN; i++) {
        if (s_conn[i].bytes == 0U && s_conn[i].handle == 0U) continue;
        used++;
        if (s_conn[i].handle != 0U) live++;
        total += s_conn[i].bytes;
        segs  += s_conn[i].segs;
        if (s_conn[i].t_first != 0U && s_conn[i].t_first < first)
            first = s_conn[i].t_first;
        if (s_conn[i].t_last > last) last = s_conn[i].t_last;

        u32 ms = (s_conn[i].t_last > s_conn[i].t_first)
               ? (s_conn[i].t_last - s_conn[i].t_first) : 0U;
        CLI_PRINTF_N("  conn %u: h=%u %u bytes %u segs %u ms %u kbps",
                     (u32)i, (u32)s_conn[i].handle,
                     s_conn[i].bytes, s_conn[i].segs, ms,
                     kbps_of(s_conn[i].bytes, ms));
    }

    /* The SUM is measured over the wall time the connections OVERLAPPED, not
     * over the sum of their individual spans: they run together, so adding
     * the spans would divide the bytes by about N times too much and report a
     * quarter of the real rate. */
    u32 wall = (last > first && first != 0xFFFFFFFFU) ? (last - first) : 0U;
    CLI_PRINTF_N("  accepted=%u refused=%u  live=%u  slots_used=%u/%u",
                 (u32)s_accepted, (u32)s_refused, (u32)live,
                 (u32)used, (u32)SINK_MAX_CONN);
    CLI_PRINTF_N("  TOTAL %u bytes  %u segs  %u ms  %u kbps",
                 total, segs, wall, kbps_of(total, wall));
    if (s_refused > 0U)
        CLI_PUT_N("  refused > 0: more connections than slots, not a pool fault");
}

/* ============================================================================
 * CLI
 * ========================================================================== */
#if ENABLE_CLI
static bool sink_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("msink     : start [port] | stats | stop");
        return false;
    }
    if (ca_strncmp(p, "msink", 5) != 0) return false;
    p = ca_skip_sp(p + 5);

    if (ca_strncmp(p, "start", 5) == 0) {
        p = ca_skip_sp(p + 5);
        u16 port = (*p != '\0') ? (u16)ca_strtol(p, 10) : SINK_PORT_DEFAULT;
        if (port == 0U) port = SINK_PORT_DEFAULT;
        if (!s_dhcp_bound) {
            CLI_PUT_N("msink: no IP yet - wait for DHCP BOUND");
            return true;
        }
        if (sink_start(port)) CLI_PRINTF_N("msink: listening on %u", (u32)port);
        else                  CLI_PUT_N("msink: already started, or listen failed");
        return true;
    }
    if (ca_strncmp(p, "stats", 5) == 0) {
        CLI_PRINTF_N("msink port=%u listening=%s",
                     (u32)s_port, s_listen_h ? "yes" : "no");
        sink_report();
        return true;
    }
    if (ca_strncmp(p, "stop", 4) == 0) {
        sink_stop();
        CLI_PUT_N("msink: stopped");
        sink_report();
        return true;
    }

    CLI_PUT_N("msink     : start [port] | stats | stop");
    return false;
}

static ca_cli_node_t s_sink_cli_node = { sink_cli_handler, NULL };
#endif

/* ============================================================================
 * DHCP
 * ========================================================================== */
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
    s_ip         = ip;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_arp_send_garp();

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    /* The PC peer must be running already for this message to exist at all -
     * it is what served the lease. So the remaining step is this one, and it
     * is worth saying which, because the natural guess is the wrong order. */
    ca_stdout_put_n("msink will start listening on its own in 2 s");
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

/* ============================================================================
 * Net task
 * ========================================================================== */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x85U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX85: parallel TCP sink (buffer pool sharing) ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);
    ca_stdout_printf_n("pool=%u  conns=%u  rx_ring=%u  inflight=%u",
        (u32)NET_BUF_COUNT, (u32)TCP_MAX_CONN,
        (u32)TCP_RX_RING_DEPTH, (u32)TCP_MAX_INFLIGHT);

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
    u32 bound_at = 0U;

    while (1) {
        ca_eth_link_poll();
        ca_led_toggle(3U);

        /* Start listening by itself, a couple of seconds after the address
         * arrives.
         *
         * WHY AUTOMATIC AND NOT A TYPED COMMAND
         *
         * The PC peer has to be running BEFORE the board can get a lease - it
         * is the peer that serves DHCP - so the peer is always already
         * waiting, retrying its connections, by the time this example knows
         * its own address. Requiring `msink start` at that point adds a human
         * to a sequence that has no other reason to need one, and the bench
         * would have to type it through the serial console with the timing
         * that implies. Starting here needs neither.
         *
         * The delay lets the address settle into the ARP service and the
         * gratuitous ARP go out, so the peer's first SYN is not answered from
         * an address the network has not learnt yet.
         *
         * `msink start` still works, for a port other than the default or to
         * begin a second run: sink_start() refuses while a listener is up, so
         * this cannot fight with it.
         *
         * Run from here rather than from on_dhcp_bound() because that runs in
         * the net task's own callback - opening a listener from inside the
         * stack's receive path is the kind of re-entry that works until the
         * day it does not. */
        if (s_dhcp_bound && s_listen_h == 0U) {
            u32 now = ca_os_tick_ms();
            if (bound_at == 0U) {
                bound_at = now;
            } else if ((now - bound_at) >= SINK_AUTOSTART_MS) {
                if (sink_start(s_port)) {
                    ca_stdout_printf_n(
                        "msink: listening on %u (started automatically)",
                        (u32)s_port);
                } else {
                    ca_stdout_put_n("msink: automatic start FAILED");
                }
                bound_at = 0U;
            }
        } else if (!s_dhcp_bound) {
            bound_at = 0U;        /* lease gone: re-arm for the next one */
        }

        ca_os_sleep(500);
    }
    return 0U;
}

/* ============================================================================
 * GPIO ETH — RMII
 * ========================================================================== */
static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
        /* RMII_TXD1: PB15 on the H563 (UM3115, JP6 ON / SB10 OFF), PB13 on
         * the H753 and F767. Hardcoding PB13 leaves TXD1 unconnected on an
         * H563 - every frame still counts as transmitted and tx_fail stays 0,
         * but half the data bits never reach the PHY. */
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
 * ========================================================================== */
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
        ca_cli_register(&s_sink_cli_node);
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");

    ca_ipv4_t zero = { .addr = 0U };
    ca_arp_service_init(s_mac, zero);
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();

    ca_os_job("net",     CA_TH_LEVEL6, task_net,     NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m85a, "=== Example 85: parallel TCP sink ===");
PRIVATE_STRING(m85b, "DISABLED - requires EN_ETH / EN_NET / EN_TCP_SERVICE / EN_ARP_SERVICE");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m85a);
    PUT_N(&priv_m85b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_TCP_SERVICE && EN_ARP_SERVICE */
