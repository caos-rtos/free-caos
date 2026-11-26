// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 136 -- caOS IP fragmentation on TX under load (clone of ex48)
 *
 * Hardware: any Nucleo with Ethernet (H753, F767, H563, F439, H773 ...)
 *
 * Features:
 *   1. DHCP client, ARP service, ICMP echo reply (ping)
 *   2. UDP echo server on port 7961, SILENT (ex48 logs every datagram: under
 *      a burst the console, not the network, becomes the bottleneck)
 *   3. ip_frag service (EN_IP_FRAG): every datagram over the MTU of the
 *      output iface leaves as fragments. 'ip mtu 512' simulates a radio link
 *   4. "e136 start": up to 4 board tasks fire UDP bursts at the PC at the
 *      same time, through ca_udp_client. This is the load the PC alone cannot
 *      make: several local senders competing for the fragment queue and the
 *      net_buf pool (ip_frag_mtu.md sec. 4.4)
 *   5. "e136 conn": the same tasks call ca_udp_client_connect() at the same
 *      moment, with the peer removed from the ARP cache so that every connect
 *      blocks in the resolve together. Every round must hand out distinct,
 *      non-zero handles (two tasks sharing one slot = the slot race). Run it
 *      first after a reset to also cover the lazy init of the module.
 *   6. "e136 orphan" (task_reaper): a task opens a client on a fixed port and
 *      ends without closing it. While it lives nobody else can bind the port;
 *      within ~1 s of its end the handle is closed and the port is free.
 *
 * A run is ONE deterministic pass and ends with a verdict:
 *        RUN RESULT : OK  (1/1)    every send accepted and the pool whole again
 *        RUN RESULT : FAIL  (0/1)  a send given up, a task stuck, or a leak
 * Datagrams the service drops after accepting them (pool, queue) are counted
 * and printed, not failed: under pressure dropping is the contract. Whether
 * what arrived is intact is the PC's verdict (frag_rx_pc.py).
 *
 * CLI:
 *   e136 start [tasks] [count] [size] [port]   defaults 4 100 1800 7962
 *   e136 conn  [tasks] [rounds]                defaults 4 20 (no PC side)
 *   e136 orphan                                a task opens a client and ends
 *   e136 info
 *
 * Test from PC (192.168.10.1, bench network):
 *   python3 examples/example_136/frag_rx_pc.py 7962 &     # receiver
 *   ip mtu 512            (board CLI)
 *   e136 start 4 100 1800 (board CLI)
 *
 * Payload of every datagram: [0]=task  [1]=0x36  [2..5]=seq (BE)
 * [6..7]=run (BE), then byte i = (i*7 + seq + task*13) & 0xFF.
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_NET_DHCP && EN_DHCP_SERVICE && EN_UDP_SERVER && \
    EN_IP_DEFRAG && EN_IP_FRAG && EN_UDP_CLIENT

/* ============================================================================
 * Constants
 * ============================================================================ */
#define UDP_SRV_PORT        7961U
#define MAX_CLIENTS         2U
#define CLIENT_TIMEOUT_MS   30000U

#define E136_MAX_TASKS      4U          /* = UDPCLI_MAX_HANDLES */
#define E136_MAX_SIZE       1814U       /* largest datagram in one net_buf */
#define E136_HDR            8U
#define E136_DEF_TASKS      4U
#define E136_DEF_COUNT      100U
#define E136_DEF_SIZE       1800U
#define E136_DEF_PORT       7962U
#define E136_SEND_TRIES     200U        /* x 1 ms before a send is given up */
#define E136_SETTLE_MS      2000U       /* pool back to the baseline within */
#define E136_DEF_ROUNDS     20U
#define E136_CONN_ARP_MS    2000U       /* arp_timeout_ms of every connect */
#define E136_CONN_WAIT_MS   5000U       /* a connect round must end within */

#define E136_MODE_BURST     0U
#define E136_MODE_CONN      1U

#define ETH_AF              11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t s_mac = CA_EXAMPLE_MAC(136);

static volatile ca_ipv4_t s_ip   = { .addr = 0U };
static volatile ca_ipv4_t s_gw   = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = CA_EXAMPLE_MAC(136),
    .link_cb  = on_link_change,
};

/* ============================================================================
 * Burst state
 * ============================================================================ */
typedef struct {
    u8  handle;
    u32 sent;
    u32 retry;
    u32 fail;
    u32 max_tries;      /* longest run of refusals for one datagram */
    u32 max_wait_ms;    /* longest time to get one datagram accepted */
} e136_worker_t;

static e136_worker_t  s_w[E136_MAX_TASKS];
static u8             s_pay[E136_MAX_TASKS][E136_MAX_SIZE];
static ca_semaphore_t s_go[E136_MAX_TASKS];
static ca_semaphore_t s_done;

static volatile bool  s_busy;
static u32 s_tasks = E136_DEF_TASKS;
static u32 s_count = E136_DEF_COUNT;
static u32 s_size  = E136_DEF_SIZE;
static u32 s_port  = E136_DEF_PORT;
static u32 s_runs, s_runs_ok;
static volatile u32 s_mode = E136_MODE_BURST;
static ca_udp_client_cfg_t s_ccfg;     /* shared by the workers in conn mode */

/* ============================================================================
 * DHCP
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

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    s_ip         = ip;
    s_gw         = gw;
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
    ca_arp_send_garp();
    ca_stdout_put_n("ex136: idle - \"e136 start\" runs one burst");
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * UDP echo server - silent
 * ============================================================================ */
static void on_rx(u8 cid, ca_ipv4_t ip, u16 port, const u8 *pld, u16 len)
{
    (void)ip; (void)port;
    ca_udp_server_send(cid, pld, len);
}

static ca_udp_server_cfg_t srv_cfg = {
    .port              = UDP_SRV_PORT,
    .max_clients       = MAX_CLIENTS,
    .client_timeout_ms = CLIENT_TIMEOUT_MS,
    .local_mac         = CA_EXAMPLE_MAC(136),
    .local_ip          = { .addr = 0U },
    .on_rx             = on_rx,
    .on_connect        = NULL,
    .on_disconnect     = NULL,
};

/* ============================================================================
 * Burst workers
 * ============================================================================ */
static void fill_payload(u8 *p, u32 task, u32 seq, u32 run, u32 size)
{
    p[0] = (u8)task;
    p[1] = 0x36U;
    p[2] = (u8)(seq >> 24); p[3] = (u8)(seq >> 16);
    p[4] = (u8)(seq >> 8);  p[5] = (u8)seq;
    p[6] = (u8)(run >> 8);  p[7] = (u8)run;
    for (u32 i = E136_HDR; i < size; i++)
        p[i] = (u8)((i * 7U + seq + task * 13U) & 0xFFU);
}

static u32 task_burst(void *arg)
{
    u32 id = (u32)(uintptr_t)arg;
    e136_worker_t *w = &s_w[id];

    while (1) {
        ca_sem_wait(&s_go[id]);
        if (s_mode == E136_MODE_CONN) {
            /* All workers are released together: concurrent connects */
            w->handle = ca_udp_client_connect(&s_ccfg);
            ca_sem_post(&s_done);
            continue;
        }
        for (u32 seq = 0U; seq < s_count; seq++) {
            fill_payload(s_pay[id], id, seq, s_runs, s_size);
            u32 tries = 0U;
            const u32 t_in = ca_os_tick_ms();
            /* false = the fragment queue or the pool is full right now:
             * back off 1 ms and let the ip_frag service drain it. */
            while (!ca_udp_client_send(w->handle, s_pay[id], (u16)s_size)) {
                if (++tries >= E136_SEND_TRIES) break;
                w->retry++;
                ca_os_sleep(1U);
            }
            /* The wait includes the time blocked inside the send: ip_frag
             * queues a task in FIFO order while its input queue is full. */
            const u32 waited = ca_os_tick_ms() - t_in;
            if (tries > w->max_tries)    w->max_tries = tries;
            if (waited > w->max_wait_ms) w->max_wait_ms = waited;
            if (tries >= E136_SEND_TRIES) w->fail++;
            else                          w->sent++;
        }
        ca_sem_post(&s_done);
    }
    return 0U;
}

/* ============================================================================
 * One pass, one verdict. Runs in the cli_cmd task of "e136 start": a run
 * task of its own would cost a stack the H773 arena does not have to spare.
 * ============================================================================ */
/* Every net_buf is either free or held by the driver (RX ring, and TX
 * descriptors not reclaimed yet: the ETH driver frees a sent buffer on a
 * LATER transmission, so a quiet link keeps the last fragments). */
static u16 pool_missing(void)
{
    u16 fc = ca_net_buf_free_count();
    u16 held = ca_eth_pool_held_count();
    return (fc + held >= NET_BUF_COUNT) ? 0U : (u16)(NET_BUF_COUNT - fc - held);
}

static void e136_run(void)
{
    {
        s_runs++;

        ca_ipv4_t peer = s_gw;
        ca_stdout_printf_n("e136 run %u: tasks=%u count=%u size=%u mtu=%u -> %u.%u.%u.%u:%u",
            s_runs, s_tasks, s_count, s_size, (u32)ca_net_iface_mtu(0U),
            peer.v[0], peer.v[1], peer.v[2], peer.v[3], s_port);

        u16 base_free = ca_net_buf_free_count();
        u16 base_miss = pool_missing();
        ca_ip_frag_stats_reset();
        ca_net_buf_pressure_reset();

        /* Connect one after the other: the handles are taken here, before
         * the workers run, so the burst itself is the only contention. */
        bool ok = true;
        u32 opened = 0U;
        for (u32 i = 0U; i < s_tasks; i++) {
            ca_udp_client_cfg_t cfg = {
                .server_ip      = peer,
                .server_port    = (u16)s_port,
                .local_port     = 0U,
                .arp_timeout_ms = 2000U,
                .local_mac      = s_mac,
                .local_ip       = s_ip,
                .on_rx          = NULL,
            };
            s_w[i].handle = ca_udp_client_connect(&cfg);
            s_w[i].sent = s_w[i].retry = s_w[i].fail = 0U;
            s_w[i].max_tries = s_w[i].max_wait_ms = 0U;
            if (s_w[i].handle == 0U) {
                ca_stdout_printf_n("e136 task %u: connect FAILED", i);
                ok = false;
                break;
            }
            opened++;
        }

        u32 t0 = (u32)ca_os_tick_64_ms();
        u32 finished = 0U;
        if (ok) {
            for (u32 i = 0U; i < s_tasks; i++) ca_sem_post(&s_go[i]);
            /* Bounded: 50 ms per datagram is far beyond any healthy pass */
            u32 limit = 5000U + s_count * 50U;
            for (; finished < s_tasks; finished++) {
                if (!ca_sem_timedwait(&s_done, limit)) break;
            }
        }
        u32 dt = (u32)ca_os_tick_64_ms() - t0;

        for (u32 i = 0U; i < opened; i++) ca_udp_client_disconnect(s_w[i].handle);

        if (ok && finished < s_tasks) {
            ca_stdout_printf_n("e136: %u of %u tasks never finished", s_tasks - finished, s_tasks);
            ok = false;
        }

        /* The fragments still queued own buffers: give them time */
        u16 miss = pool_missing();
        for (u32 ms = 0U; ms < E136_SETTLE_MS && miss > base_miss; ms += 10U) {
            ca_os_sleep(10U);
            miss = pool_missing();
        }
        u16 now_free = ca_net_buf_free_count();

        u32 sent = 0U, fail = 0U;
        for (u32 i = 0U; i < opened; i++) {
            ca_stdout_printf_n("e136 task %u: sent=%u retry=%u fail=%u max_tries=%u max_wait=%u ms",
                i, s_w[i].sent, s_w[i].retry, s_w[i].fail, s_w[i].max_tries, s_w[i].max_wait_ms);
            sent += s_w[i].sent;
            fail += s_w[i].fail;
        }

        ca_ip_frag_stats_t fs;
        ca_ip_frag_stats(&fs);
        ca_net_buf_pressure_t pr;
        ca_net_buf_pressure(&pr);

        ca_stdout_printf_n("e136 frag: dgrams=%u frags=%u wait_tx=%u in_hwm=%u",
            fs.dgrams, fs.frags, fs.wait_tx, (u32)fs.in_hwm);
        ca_stdout_printf_n("e136 frag drop: queue=%u pool=%u big=%u tx=%u bad=%u df=%u",
            fs.drop_queue, fs.drop_pool, fs.drop_big, fs.drop_tx, fs.drop_bad, fs.drop_df);
        ca_stdout_printf_n("e136 pool: free before=%u after=%u held=%u total=%u min=%u alloc_fail=%u/%u",
            (u32)base_free, (u32)now_free, (u32)ca_eth_pool_held_count(), (u32)NET_BUF_COUNT,
            (u32)pr.free_min, pr.alloc_fail, pr.alloc_calls);
        ca_stdout_printf_n("e136 sent=%u fail=%u in %u ms", sent, fail, dt);

        if (fail != 0U) ok = false;
        if (miss > base_miss) {
            ca_stdout_printf_n("e136: %u net_buf never came back", (u32)(miss - base_miss));
            ok = false;
        }
        if (fs.drop_bad != 0U || fs.drop_big != 0U) ok = false;

        if (ok) s_runs_ok++;
        ca_stdout_printf_n("  RUN RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
    }
}

/* ============================================================================
 * Concurrent connect: every round the workers open one client each at the
 * same moment, then the run closes them all. Verdict on the handles only.
 * ============================================================================ */
static void e136_conn(u32 rounds)
{
    s_runs++;

    ca_ipv4_t peer = s_gw;
    ca_stdout_printf_n("e136 conn run %u: tasks=%u rounds=%u -> %u.%u.%u.%u",
        s_runs, s_tasks, rounds, peer.v[0], peer.v[1], peer.v[2], peer.v[3]);

    ca_udp_client_cfg_t cfg = {
        .server_ip      = peer,
        .server_port    = (u16)s_port,
        .local_port     = 0U,
        .arp_timeout_ms = E136_CONN_ARP_MS,
        .local_mac      = s_mac,
        .local_ip       = s_ip,
        .on_rx          = NULL,
    };
    s_ccfg = cfg;

    bool ok   = true;
    u32  dup  = 0U;
    u32  fail = 0U;
    u32  done = 0U;
    u32  shown = 0U;

    s_mode = E136_MODE_CONN;
    for (u32 r = 0U; r < rounds; r++) {
#if EN_ARP_SERVICE
        /* Empty cache: every connect blocks in the resolve, all together */
        (void)ca_arp_del_ip(peer);
#endif
        for (u32 i = 0U; i < s_tasks; i++) s_w[i].handle = 0U;
        for (u32 i = 0U; i < s_tasks; i++) ca_sem_post(&s_go[i]);

        u32 fin = 0U;
        for (; fin < s_tasks; fin++) {
            if (!ca_sem_timedwait(&s_done, E136_CONN_WAIT_MS)) break;
        }
        if (fin < s_tasks) {
            ca_stdout_printf_n("e136 conn round %u: %u of %u connects never returned",
                r, s_tasks - fin, s_tasks);
            ok = false;
            break;
        }

        u32 rdup = 0U, rfail = 0U;
        for (u32 i = 0U; i < s_tasks; i++) {
            if (s_w[i].handle == 0U) { rfail++; continue; }
            for (u32 j = 0U; j < i; j++)
                if (s_w[j].handle == s_w[i].handle) { rdup++; break; }
        }
        if ((rdup != 0U || rfail != 0U) && shown < 3U) {
            shown++;
            ca_stdout_printf_n("e136 conn round %u: handles %u %u %u %u  dup=%u fail=%u",
                r, (u32)s_w[0].handle, (u32)s_w[1].handle,
                (u32)s_w[2].handle, (u32)s_w[3].handle, rdup, rfail);
        }
        dup  += rdup;
        fail += rfail;
        done++;

        /* A shared handle is closed twice: the second call is a no-op */
        for (u32 i = 0U; i < s_tasks; i++)
            if (s_w[i].handle != 0U) ca_udp_client_disconnect(s_w[i].handle);
    }
    s_mode = E136_MODE_BURST;

    ca_stdout_printf_n("e136 conn: rounds=%u/%u dup=%u fail=%u", done, rounds, dup, fail);
    if (dup != 0U || fail != 0U) ok = false;

    if (ok) s_runs_ok++;
    ca_stdout_printf_n("  RUN RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
}

/* ============================================================================
 * Orphan client (task_reaper): what a task that ends leaves open goes away
 * ============================================================================ */
#define E136_ORPH_PORT      7970U       /* local port of the orphan client */
#define E136_ORPH_WAIT_MS   2500U       /* reaper runs once a second */

static ca_semaphore_t s_orph_up;       /* the orphan has its handle */
static ca_semaphore_t s_orph_end;      /* the command lets it end */
static volatile u8    s_orph_h;

static void e136_orph_rx(u16 buf_idx) { ca_net_buf_free(buf_idx); }

static u32 task_orphan(void *arg)
{
    (void)arg;
    ca_udp_client_cfg_t cfg = {
        .server_ip      = s_gw,
        .server_port    = 9U,           /* discard */
        .local_port     = E136_ORPH_PORT,
        .arp_timeout_ms = E136_CONN_ARP_MS,
        .local_mac      = s_mac,
        .local_ip       = s_ip,
        .on_rx          = NULL,
    };
    s_orph_h = ca_udp_client_connect(&cfg);
    ca_sem_post(&s_orph_up);
    (void)ca_sem_timedwait(&s_orph_end, 5000U);
    return 0U;                          /* ends WITHOUT disconnect */
}

static void e136_orphan(void)
{
    s_runs++;
    while (ca_sem_trywait(&s_orph_up)) { }
    while (ca_sem_trywait(&s_orph_end)) { }
    s_orph_h = 0U;

    const bool made = ca_os_job("e136or", CA_TH_LEVEL3, task_orphan, NULL, STACK_SMALL,
                                CA_JOB_STD, CA_FAULT_ASSERT) != CA_TASK_HANDLE_NONE;
    const bool up = made && ca_sem_timedwait(&s_orph_up, 5000U) && s_orph_h != 0U;

    /* owner alive: another task (this command) may not take its port */
    const bool refused = up && !ca_net_proto_bind_udp(E136_ORPH_PORT, e136_orph_rx);
    ca_sem_post(&s_orph_end);

    /* owner gone: the handle is closed by the reaper (send refuses) */
    u32 t = 0U;
    const u8 one = 0x36U;
    while (up && ca_udp_client_send(s_orph_h, &one, 1U) && t < E136_ORPH_WAIT_MS) {
        ca_os_sleep(50U);
        t += 50U;
    }
    const bool closed = up && t < E136_ORPH_WAIT_MS;

    /* and the port is free for anyone */
    const bool taken = closed && ca_net_proto_bind_udp(E136_ORPH_PORT, e136_orph_rx);
    if (taken) (void)ca_net_proto_bind_udp(E136_ORPH_PORT, NULL);

    const bool ok = up && refused && closed && taken;
    ca_stdout_printf_n("e136 orphan: handle=%u refused-while-alive=%s closed-after=%u ms "
        "port-free=%s", (u32)s_orph_h, refused ? "yes" : "NO", t, taken ? "yes" : "NO");
    if (ok) s_runs_ok++;
    ca_stdout_printf_n("  RUN RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
}

/* ============================================================================
 * CLI "e136"
 * ============================================================================ */
#if ENABLE_CLI
static const char *parse_u32(const char *p, u32 *out, bool *got)
{
    u32 v = 0U;
    *got = false;
    while (*p >= '0' && *p <= '9') { v = v * 10U + (u32)(*p - '0'); p++; *got = true; }
    if (*got) *out = v;
    return ca_skip_sp(p);
}

/* e136 — the line arrives without the command word */
static bool e136_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "start", 5) == 0) {
        if (!s_dhcp_bound) { CLI_PUT_N("e136: no DHCP lease yet"); return true; }
        if (s_busy)        { CLI_PUT_N("e136: a run is already going"); return true; }
        u32 t = E136_DEF_TASKS, c = E136_DEF_COUNT, s = E136_DEF_SIZE, po = E136_DEF_PORT;
        bool got;
        p = ca_skip_sp(p + 5);
        p = parse_u32(p, &t, &got);
        p = parse_u32(p, &c, &got);
        p = parse_u32(p, &s, &got);
        (void)parse_u32(p, &po, &got);
        if (t < 1U || t > E136_MAX_TASKS || c < 1U || c > 100000U ||
            s < E136_HDR || s > E136_MAX_SIZE || po < 1U || po > 65535U) {
            CLI_PRINTF_N("e136: tasks 1..%u, count 1..100000, size %u..%u, port 1..65535",
                E136_MAX_TASKS, E136_HDR, E136_MAX_SIZE);
            return true;
        }
        s_tasks = t; s_count = c; s_size = s; s_port = po;
        s_busy = true;
        e136_run();
        s_busy = false;
        return true;
    }
    if (ca_strncmp(p, "conn", 4) == 0) {
        if (!s_dhcp_bound) { CLI_PUT_N("e136: no DHCP lease yet"); return true; }
        if (s_busy)        { CLI_PUT_N("e136: a run is already going"); return true; }
        u32 t = E136_DEF_TASKS, r = E136_DEF_ROUNDS;
        bool got;
        p = ca_skip_sp(p + 4);
        p = parse_u32(p, &t, &got);
        (void)parse_u32(p, &r, &got);
        if (t < 2U || t > E136_MAX_TASKS || r < 1U || r > 10000U) {
            CLI_PRINTF_N("e136: conn tasks 2..%u, rounds 1..10000", E136_MAX_TASKS);
            return true;
        }
        s_tasks = t;
        s_busy = true;
        e136_conn(r);
        s_busy = false;
        return true;
    }
    if (ca_strncmp(p, "orphan", 6) == 0) {
        if (!s_dhcp_bound) { CLI_PUT_N("e136: no DHCP lease yet"); return true; }
        if (s_busy)        { CLI_PUT_N("e136: a run is already going"); return true; }
        s_busy = true;
        e136_orphan();
        s_busy = false;
        return true;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("state : %s", !s_dhcp_bound ? "waiting for lease" : (s_busy ? "busy" : "ready"));
        CLI_PRINTF_N("last  : tasks=%u count=%u size=%u port=%u", s_tasks, s_count, s_size, s_port);
        CLI_PRINTF_N("runs  : %u  ok %u", s_runs, s_runs_ok);
        CLI_PRINTF_N("mtu   : %u", (u32)ca_net_iface_mtu(0U));
        return true;
    }
    CLI_PUT_N("e136: start [tasks] [count] [size] [port] | conn [tasks] [rounds] | orphan | info");
    return true;
}

static const ca_cli_cmd_t s_e136_cmds[] = {
    { "e136", "e136      : start [tasks] [count] [size] [port] | conn [tasks] [rounds] | orphan | info", { e136_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_e136_table = {
    "e136", s_e136_cmds, (u16)(sizeof(s_e136_cmds) / sizeof(s_e136_cmds[0])), NULL
};
#endif

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = CA_EXAMPLE_MAC(136),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX136: IP fragmentation on TX under load ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2], s_mac.v[3], s_mac.v[4], s_mac.v[5]);
    ca_stdout_printf_n("frag: in_depth=%u queue_depth=%u  net_buf=%u  udp_cli=%u",
        (u32)IP_FRAG_IN_DEPTH, (u32)IP_FRAG_QUEUE_DEPTH,
        (u32)ca_net_buf_free_count(), (u32)UDPCLI_MAX_HANDLES);

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
        /* RMII_TXD1: PB15 on the NUCLEO-H563ZI (UM3115), PB13 elsewhere */
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

    ca_sem_init(&s_done, "e136dn", 0U, E136_MAX_TASKS);
    for (u32 i = 0U; i < E136_MAX_TASKS; i++)
        ca_sem_init(&s_go[i], "e136go", 0U, 1U);
    ca_sem_init(&s_orph_up,  "e136ou", 0U, 1U);
    ca_sem_init(&s_orph_end, "e136oe", 0U, 1U);

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");

    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })
    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)

    if (!ca_ip_defrag_init(IP_DEFRAG_MAX_PAYLOAD))
        CA_SAFE_FAIL("ca_ip_defrag_init");
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)
    ca_udp_server_start(&srv_cfg);
    ca_ip_defrag_service_start();
    ca_ip_frag_service_start();
    EVAL_CLI( ca_cli_register_table(&s_e136_table); )

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    for (u32 i = 0U; i < E136_MAX_TASKS; i++)
        ca_os_job("e136tx", CA_TH_LEVEL3, task_burst, (void *)(uintptr_t)i,
                  STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m136a, "=== Example 136: caOS IP fragmentation on TX under load ===");
PRIVATE_STRING(m136b, "DISABLED - requires EN_ETH / EN_NET_DHCP / EN_UDP_SERVER / EN_UDP_CLIENT / EN_IP_DEFRAG / EN_IP_FRAG");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m136a);
    PUT_N(&priv_m136b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
