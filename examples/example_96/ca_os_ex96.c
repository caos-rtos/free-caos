// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 96 -- soak of the L2 relay USB-NCM <-> ETH (step F2)
 *
 * Step F2 of the ETH + NCM series (ex94-ex97, app_3). The relay is the one of ex95, unchanged:
 * here it is the thing under test, kept busy by iperf in both directions for
 * minutes, hours or a day. What decides is not the rate but the invariants
 * printed by the 'soak' command: buffers back in the pool once the traffic
 * stops, no double free, no task restarted, the relay still answering.
 *
 *   host (NCM)  192.168.10.200  Mh = 02:CA:05:00:00:96 (we give it, NCM desc)
 *   board       192.168.10.96   Mb = 02:00:00:00:00:96 (on both links)
 *   PC (ETH)    192.168.10.1    Mp = learned with ARP at start
 *
 * HOW
 *
 *   1. Proxy ARP: the host asking "who has .1?" and the PC asking "who has
 *      .200?" are both answered with Mb. Each side sees the board as its
 *      only neighbour.
 *   2. So the frames for the other side arrive addressed to Mb. Two MAC
 *      filters (by SOURCE MAC: Mh on usb0, Mp on eth0) hand them to the
 *      application before the stack sees them; what is for the board itself
 *      (IP .96, ARP for .96) is declined and goes on to the stack as usual.
 *   3. A relay thread dequeues, rewrites BOTH MACs (dst = the real peer,
 *      src = Mb) and sends the frame out of the other iface with
 *      ca_net_send_raw(): the IP/L4 checksums do not cover L2, so they are
 *      still right, and the MAC must not touch them (ETH CIC=00 - with CIC=11
 *      it would insert a wrong ICMP checksum).
 *
 * Hardware: NUCLEO-H753ZI (USB_NUM=1), NUCLEO-H563ZI and NUCLEO-F767ZI
 *           (USB_NUM=0). Parts without ETH report UNSUPPORTED.
 *
 * PC procedure: the NCM interface goes into a network namespace with the
 * host address, or the kernel routes host<->PC traffic internally:
 *   sudo ./tools/bench_net.sh up            # eth: 192.168.10.1/24
 *   sudo ./tools/bench_net.sh ns-usb up     # usb iface in netns, .200/24
 *   iperf -s -B 192.168.10.1 &                              # PC side server
 *   sudo ./tools/bench_net.sh ns-usb iperf 192.168.10.1 60 up 1    # host -> PC
 *   sudo ./tools/bench_net.sh ns-usb iperf 192.168.10.1 60 down 1  # PC -> host
 *   sudo ./tools/bench_net.sh ns-usb iperf 192.168.10.1 60 dual 4  # both, 4 streams
 *   board CLI: soak                         # counters and invariants, one line
 *
 * REQUIREMENTS os_config.h: EN_ETH, EN_NET, EN_USB, EN_USB_NCM, EN_NET_FILTER
 */

#include "caos.h"
#include "net_proto.h"

/* ── USB instance per part ────────────────────────────────────────────── */
#if MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS (as ex55) */
#elif MCU_ID == 563 || MCU_ID == 767
#define USB_NUM      0U    /* H563 USB_DRD_FS / F767 OTG_FS */
#endif

#if EN_ETH && EN_NET && EN_USB && EN_USB_NCM && EN_NET_FILTER && defined(USB_NUM)

#define ETH_AF         11U
#define IF_ETH         0U          /* registered first (ca_net_init) */
#define IF_USB         1U
#define RELAY_DEPTH    16U

/* ============================================================================
 * Identities
 * ============================================================================ */
static const ca_mac_t  s_mb      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x96U } };
static const ca_mac_t  s_mh      = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x96U } };
static const ca_ipv4_t s_ip      = { .v = { 192U, 168U, 10U, 96U } };
static const ca_ipv4_t s_host_ip = { .v = { 192U, 168U, 10U, 200U } };
static const ca_ipv4_t s_pc_ip   = { .v = { 192U, 168U, 10U, 1U } };
static ca_mac_t        s_mp;                   /* PC MAC, learned */
static volatile bool   s_mp_ok;

static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 96U } },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x96U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x96U } },
    .link_cb  = on_link_change,
};

/* NCM: the MAC here is the HOST side one, Mh */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x96U },
};

/* ============================================================================
 * Relay queue and counters
 * ============================================================================ */
typedef struct { u16 buf; u8 iface; u8 pad; } relay_item_t;
static CAQUEUE_STATIC_DEFINE(s_relay_q, RELAY_DEPTH, sizeof(relay_item_t));

static volatile u32 s_n2e, s_e2n, s_proxy_arp, s_drop_q, s_drop_nomac, s_tx_fail;
/* Restarts by CA_FAULT_RESTART_THREAD: a soak that passes with one of these
 * moving has survived a fault, it has not run clean. */
static volatile u32 s_net_rst, s_relay_rst;

static bool ip_eq(ca_ipv4_t a, ca_ipv4_t b) { return a.addr == b.addr; }
static bool mac_eq(const ca_mac_t *a, const ca_mac_t *b)
{
    return ca_memcmp(a->v, b->v, 6U) == 0;
}

/* ============================================================================
 * MAC filter callback - net task context, must not block.
 * true = taken (queued for the relay thread), false = the stack handles it.
 * ============================================================================ */
static bool relay_filter(u16 buf_idx, u8 iface)
{
    ca_net_buf_t        *b  = ca_net_buf_ptr(buf_idx);
    const ca_l2_frame_t *l2 = (const ca_l2_frame_t *)(b->data + b->data_off);
    const u16            et = ca_ntohs(l2->ethertype);

    if (et == ETH_P_ARP) {
        if (b->data_len < sizeof(ca_l2_frame_t) + sizeof(ca_arp_frame_t)) return false;
        const ca_arp_frame_t *arp = (const ca_arp_frame_t *)CA_L2_PAYLOAD(l2);
        if (ca_ntohs(arp->oper) != ARP_OP_REQUEST) return false;   /* replies: stack */
        ca_ipv4_t tpa = arp->tpa;
        if (ip_eq(tpa, s_ip)) return false;                         /* for the board */
        /* host asks for anything not the board / PC asks for the host */
        if (iface == IF_ETH && !ip_eq(tpa, s_host_ip)) return false;
    } else if (et == ETH_P_IP) {
        if (b->data_len < sizeof(ca_l2_frame_t) + 20U) return false;
        if (!mac_eq(&l2->dst, &s_mb)) return false;                 /* not to us */
        const ca_ip4_frame_t *ip4 = (const ca_ip4_frame_t *)CA_L2_PAYLOAD(l2);
        ca_ipv4_t dst = ip4->dst;
        if (ip_eq(dst, s_ip)) return false;                         /* for the board */
        if (iface == IF_ETH && !ip_eq(dst, s_host_ip)) return false;
    } else {
        return false;
    }

    relay_item_t it = { buf_idx, iface, 0U };
    if (!ca_queue_static_trysend(&s_relay_q.q, &it)) {
        s_drop_q++;
        ca_net_buf_free(buf_idx);
    }
    return true;
}

/* ============================================================================
 * Relay thread
 * ============================================================================ */
static void proxy_arp(u16 buf_idx, u8 iface)
{
    ca_net_buf_t         *b   = ca_net_buf_ptr(buf_idx);
    const ca_l2_frame_t  *l2  = (const ca_l2_frame_t *)(b->data + b->data_off);
    const ca_arp_frame_t *arp = (const ca_arp_frame_t *)CA_L2_PAYLOAD(l2);

    u16 tx = ca_net_buf_alloc();
    if (tx != NET_BUF_INVALID) {
        ca_net_buf_t *tb  = ca_net_buf_ptr(tx);
        u8           *out = tb->data + NET_BUF_HEADROOM;
        /* "tpa is at Mb": the board answers for the address asked about */
        u16 n = ca_net_proto_arp_build_reply(arp, s_mb, arp->tpa, out);
        if (n > 0U) {
            tb->data_off = NET_BUF_HEADROOM;
            tb->data_len = n;
            if (ca_net_send_on(iface, tx)) s_proxy_arp++; else s_tx_fail++;
        } else {
            ca_net_buf_free(tx);
        }
    }
    ca_net_buf_free(buf_idx);
}

static void relay_ip(u16 buf_idx, u8 iface)
{
    ca_net_buf_t  *b  = ca_net_buf_ptr(buf_idx);
    ca_l2_frame_t *l2 = (ca_l2_frame_t *)(b->data + b->data_off);

    u8 out_if;
    if (iface == IF_USB) {                      /* host -> PC */
        if (!s_mp_ok) { s_drop_nomac++; ca_net_buf_free(buf_idx); return; }
        l2->dst = s_mp;
        out_if  = IF_ETH;
    } else {                                    /* PC -> host */
        l2->dst = s_mh;
        out_if  = IF_USB;
    }
    l2->src = s_mb;

    /* The iface owns the buffer from here, whatever it returns */
    if (ca_net_send_raw(out_if, buf_idx)) {
        if (out_if == IF_ETH) s_n2e++; else s_e2n++;
    } else {
        s_tx_fail++;
    }
}

static bool s_relay_up;

static u32 task_relay(void *arg)
{
    (void)arg;
    if (s_relay_up) s_relay_rst++;
    s_relay_up = true;

    /* The PC's MAC, before the eth0 filter can be armed */
    while (!s_mp_ok) {
        if (ca_arp_resolve(s_pc_ip, &s_mp, 2000U)) {
            s_mp_ok = true;
            ca_net_filter_mac_set(1U, &s_mp, relay_filter);
            ca_stdout_printf_n("RELAY PC %02X:%02X:%02X:%02X:%02X:%02X",
                s_mp.v[0],s_mp.v[1],s_mp.v[2],s_mp.v[3],s_mp.v[4],s_mp.v[5]);
        }
    }
    ca_stdout_printf_n("Relay ready -- host .200 (usb0) <-> PC .1 (eth0)");

    while (1) {
        relay_item_t it;
        if (!ca_queue_static_receive(&s_relay_q.q, &it, 1000U)) continue;
        ca_net_buf_t        *b  = ca_net_buf_ptr(it.buf);
        const ca_l2_frame_t *l2 = (const ca_l2_frame_t *)(b->data + b->data_off);
        if (ca_ntohs(l2->ethertype) == ETH_P_ARP) proxy_arp(it.buf, it.iface);
        else                                      relay_ip(it.buf, it.iface);
    }
    return 0;
}

/* ============================================================================
 * Net task
 * ============================================================================ */
/* Set once the one-shot init below is done. CA_FAULT_RESTART_THREAD runs
 * task_net again from the top after a safeguard: USB, the second iface and
 * the protocols are already up then, and redoing them fails (USB "already in
 * use") - the task returned and nobody drained the RX queue any more. */
static bool s_net_up;

static u32 task_net(void *arg)
{
    (void)arg;
    if (s_net_up) {
        s_net_rst++;
        ca_stdout_printf_n("Net task restarted -- init kept");
        goto rx_loop;
    }
    ca_os_sleep(300);

    ca_stdout_printf_n("\n\n=== EX96: soak of the L2 relay USB-NCM <-> ETH ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mb.v[0],s_mb.v[1],s_mb.v[2],s_mb.v[3],s_mb.v[4],s_mb.v[5]);
    ca_stdout_printf_n("IP   %u.%u.%u.%u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3]);

    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_printf_n("[USB] init FAILED"); return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_printf_n("[USB] add NCM FAILED"); return 1U;
    }
    ca_usb_dev_mng_start();
    if (!ca_net_register_iface(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_printf_n("[NET] register usb0 FAILED"); return 1U;
    }

    ca_net_proto_init(&net_cfg);

    /* host side filter now; the PC side once its MAC is known (task_relay) */
    ca_net_filter_mac_set(0U, &s_mh, relay_filter);

    for (u8 i = 0U; i < ca_net_iface_count(); i++) {
        const ca_net_iface_t *f = ca_net_iface_at(i);
        ca_stdout_printf_n("iface %u %s csum=%s", (u32)f->id, f->name,
                           f->csum_offload ? "hw" : "sw");
    }

    ca_os_sleep(500);
    ca_eth_link_poll();
    ca_arp_send_garp();
    ca_os_sleep(50);

    ca_stdout_printf_n("Net task ready");

    s_net_up = true;

rx_loop:
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0;
}

/* ============================================================================
 * CLI: soak
 *
 * Every number the bench compares between two reads, as 'soak k=v' lines: the relay
 * counters, the net_buf pool (as 'eth pool'), the heap, the restarts. The
 * pool is balanced when free + held == total with the traffic stopped.
 * ============================================================================ */
#if ENABLE_CLI
static bool soak_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("soak      : relay counters, pool, heap, restarts");
        return false;
    }
    if (ca_strncmp(p, "soak", 4) != 0) return false;
    ca_net_buf_pressure_t pr;
    ca_net_buf_pressure(&pr);
    /* Four lines, not one: the CLI print buffer is ~128 bytes and a single
     * line overflowed it (truncated as '~OVF'). 'pc=' closes the last one. */
    CLI_PRINTF_N("soak n2e=%u e2n=%u proxy_arp=%u drop_q=%u",
                 s_n2e, s_e2n, s_proxy_arp, s_drop_q);
    CLI_PRINTF_N("soak drop_nomac=%u tx_fail=%u afail=%u dfree=%u",
                 s_drop_nomac, s_tx_fail, pr.alloc_fail,
                 ca_net_buf_double_free_count());
    CLI_PRINTF_N("soak pool=%u held=%u total=%u low=%u heap=%u",
                 (u32)pr.free_now, (u32)ca_eth_pool_held_count(), (u32)pr.total,
                 (u32)pr.free_min, ca_memory_get_avail_memory());
    CLI_PRINTF_N("soak net_rst=%u relay_rst=%u pc=%s",
                 s_net_rst, s_relay_rst, s_mp_ok ? "known" : "unknown");
    return true;
}
static ca_cli_node_t s_soak_cli = { soak_cli_handler, NULL };
#endif

/* ============================================================================
 * Link callback / monitor / heartbeat
 * ============================================================================ */
static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) {
        ca_eth_link_poll();
        ca_os_sleep(500);
    }
    return 0;
}

static u32 task_heartbeat(void *arg)
{
    (void)arg;
    while (1) {
        ca_led_toggle(3U);
        ca_os_sleep(500);
    }
    return 0;
}

/* ============================================================================
 * GPIO ETH RMII (AF11) - same wiring as ex33
 * ============================================================================ */
static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
        /* RMII_TXD1: NUCLEO-H563ZI (UM3115) uses PB15 (JP6 ON); the others PB13 */
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
        ca_cli_register(&s_soak_cli);
    )

    ca_led_init();
    ca_led_off(1U);
    ca_led_off(3U);

    /* iface 0: ETH. usb0 joins from the net task, once USB is up. */
    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg))) {
        CA_SAFE_FAIL("ca_net_init");
    }
    ca_arp_service_init(s_mb, s_ip);
}

void ca_os_main(void)
{
    /* The task that sends the ARP requests: without it ca_arp_resolve()
     * only ever finds what the cache learned from incoming traffic. */
    ca_arp_service_start();
    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("relay",     CA_TH_LEVEL4, task_relay,     NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m96a, "=== Example 96 ===");
PRIVATE_STRING(m96b, "needs ETH and USB device (EN_ETH / EN_NET / EN_USB / EN_USB_NCM / EN_NET_FILTER)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m96a);
    PUT_N(&priv_m96b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
