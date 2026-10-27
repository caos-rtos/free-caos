// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 79 -- caOS iperf TCP client/server over USB-NCM
 *
 * NCM variant of example_44 (same service on ETH). The application logic is
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically
 *   2. ARP service
 *   3. iperf service (CLI registered by the service):
 *        iperf client <ip> [port=5001] [dur_s=10]
 *        iperf server [port=5001] [idle_s=30]
 *   4. ICMP echo reply (ping from the board)
 *   5. Diagnostics via CLI: "net" | "tcp list"
 *
 * MIND THE NUMBERS: USB Full Speed is 12 Mbit/s of GROSS signalling, from
 * which USB overhead, NCM and Ethernet encapsulation have to be taken away.
 * Do not expect the ~27-33 Mbps measured on ETH in ex44: that comparison makes
 * no sense, the bottleneck here is the bus, not the stack.
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * Peer PC:
 *   sudo python3 examples/example_79/iperf_peer_ncm.py --mode server
 *   sudo python3 examples/example_79/iperf_peer_ncm.py --mode client
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_TCP_SERVICE 1 | EN_IPERF_SERVICE 1 |
 *                           EN_ARP_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#include "iperf_service.h"

/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549) — UM2861 §6.4: LD1 green PC7, LD2 blue PB7, LD3 red PG2. */
#define USB_NUM      0U

#elif MCU_ID == 753
/* NUCLEO-H753ZI (MB1364) — UM2407 §6.5: LD1 green PB0, LD2 yellow PE1,
 * LD3 red PB14. NB PB7 is the LD2 of the U575, NOT of this board. */
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U

#elif MCU_ID == 563
/* NUCLEO-H563ZI (MB1404) — UM3115 §6.4: LD1 green PB0, LD2 yellow PF4, LD3 red PG4. */
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
 * parts; this example still overflows by ~1.5 KB. What sits underneath is
 * the pair of per-EP USB queues in hw/f4xx/drivers/usb_fs_f4xx.c, 13 KB
 * each: 26 KB, a fifth of the RAM, before any of this example is counted.
 * Measured on both boards, byte for byte the same. */
#if MCU_ID == 407 || MCU_ID == 446
#define EX79_RAM_TOO_SMALL  1
#endif

#if EN_NET && EN_USB && EN_USB_NCM && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_IPERF_SERVICE && \
    !defined(EX79_RAM_TOO_SMALL)

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x79U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xB9U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * DHCP
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

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    (void)mask; (void)gw; (void)dns;
    s_ip         = ip;
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_arp_send_garp();

    ca_iperf_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    ca_stdout_put_n("ready: use 'iperf server' or 'iperf client <ip>'");
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },   /* 0.0.0.0 — updated after DHCP */
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x79U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX79: iperf TCP client/server over USB-NCM ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex44) */
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

    /* Register port 68 to receive OFFER/ACK. iperf needs no bind here: the
     * service opens its own listen/connect when the CLI command is given. */
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    /* The DORA must not start before the host has brought the interface up.
     *
     * This gate has no equivalent on ETH and is NOT a formality: the NCM link
     * goes up only when the host selects alt setting 1 (usb_ncm.c), and until
     * then ca_usb_ncm_send_frame() drops every frame on the floor and returns
     * false. A DISCOVER sent too early is not delayed, it is LOST -- and the
     * DHCP client would spend its whole retry budget (DHCP_DISCOVER_RETRY_N x
     * DHCP_OFFER_TIMEOUT_MS) talking into a closed pipe.
     *
     * But the wait CANNOT sit before the loop below: enumeration needs this
     * task to be already servicing frames, so blocking here would hold off the
     * very event being waited for (that mistake stalls enumeration outright --
     * the host reports "device not accepting address"). The gate therefore
     * lives INSIDE the loop, and the DHCP task is released the first time the
     * link is seen up.
     *
     * iperf needs no gate: it is driven by a CLI command, which by definition
     * comes long after the link is up. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            /* NB ca_dhcp_service_bind() is deliberately NOT called: it blocks,
             * and would stop ca_net_process() from running -> the OFFER would
             * never be processed -> deadlock. The IP arrives through the
             * on_dhcp_bound callback from the DHCP task (LEVEL1). */
            ca_dhcp_service_net_ready();
            ca_stdout_put_n("DHCP: starting DORA...");
        }

        /* Timed, not ca_sem_wait(): before the link is up no frame ever
         * arrives, so a plain wait would park here and the gate above would
         * never be re-evaluated. The timeout makes this loop poll the link
         * while still sleeping between frames.
         *
         * NB the 100 ms timeout costs nothing under load: as soon as frames
         * flow, the semaphore is posted and the wait returns immediately. */
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) {
            ca_net_process();
        }
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex44
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    ca_led_init();

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            /* Coming back up with a lease already taken: ask for a renew so the
             * address is re-validated instead of being trusted until T1. */
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

/* Pool/stats diagnostics on request via CLI "net" and "tcp list": the "eth"
 * commands live inside #if EN_ETH and are not built on NCM. */

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

    /* ca_net_init() is NOT called here (unlike ex44): the NCM iface only exists
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

    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_iperf_service_start(zero, s_mac);   /* IP updated by on_dhcp_bound */
    }

    /* LEVEL6, paired with the iperf service (iperf_service.c: CA_TH_LEVEL6).
     *
     * caOS levels are not fixed priorities but CPU SHARES (LEVEL4 ~12.6%,
     * LEVEL6 ~50%): net and the service that consumes what net produces have
     * to sit at the SAME level, so they split the same band evenly. If net
     * were at 4 and iperf at 6 -- or the other way round -- one of the two
     * would get four times the bandwidth of the other, and the queue between
     * them would grow on the wrong side.
     *
     * It is why ex44 (iperf) and ex45 (http) use LEVEL6 on ETH, while the
     * other examples sit at LEVEL4 with services at LEVEL0..4. */
    ca_os_job("net",       CA_TH_LEVEL6, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m79a, "=== Example 79 ===");
#ifdef EX79_RAM_TOO_SMALL
PRIVATE_STRING(m79b, "128K of RAM is not enough for iperf over NCM: short by ~1.5 KB");
#else
PRIVATE_STRING(m79b, "EN_NET / EN_USB / EN_USB_NCM / EN_IPERF_SERVICE / EN_TCP_SERVICE == 0: no tasks created");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m79a);
    PUT_N(&priv_m79b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_IPERF_SERVICE && ... */
