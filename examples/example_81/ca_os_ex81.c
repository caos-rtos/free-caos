// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 81 -- caOS IP defrag + DHCP + UDP server, over USB-NCM
 *
 * NCM variant of example_48 (same service on ETH). The application logic --
 * UDP server, CRC32 verification of the reassembled payload, echo -- is copied
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * The fragmentation test carries over as is: NCM announces wMaxSegmentSize
 * 1514 (USB_NCM_MAX_FRAME), the same L2 MTU as Ethernet, so a datagram larger
 * than 1480 bytes of IP payload is fragmented by the sender exactly as it is
 * on ETH and the reassembly path being exercised is identical.
 *
 * Functionality:
 *   1. DHCP client — gets IP automatically
 *   2. ARP service — cache + blocking resolve
 *   3. ICMP echo reply (ping)
 *   4. IP defrag service — reassembles fragmented packets (EN_IP_DEFRAG=1)
 *   5. UDP server on port 7961 — receives payload up to IP_DEFRAG_MAX_PAYLOAD
 *
 * For every datagram received (reassembled ones included) the server logs
 * length and the hardware CRC32 of the payload, then echoes the payload back,
 * so the test PC can verify the reassembly is byte-exact.
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side (needs scapy, brings its own DHCP server):
 *   sudo python3 examples/example_81/test_frag_ncm.py
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_UDP_SERVER 1 | EN_IP_DEFRAG 1 | EN_CRC 1
 */

#include "caos.h"
#include "net_proto.h"

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


/* RAM budget - parts with 128 KB (F407, F446)
 *
 * IP defrag allocates IP_DEFRAG_MAX_SESSIONS * IP_DEFRAG_MAX_PAYLOAD from the
 * heap: 8 * 1480 = 11.6 KB. On these parts the heap left after the NCM stack
 * comes up is 2.3-3.2 KB, so ca_ip_defrag_init() returns false and the example
 * halted on CA_SAFE_FAIL at boot - a fatal stop that named the function but
 * not the reason. Trimming the sessions does not rescue it either: even a
 * single 1480-byte buffer would leave under 900 bytes for everything else.
 *
 * Same 128 KB ceiling that rules out ex79, ex80 and ex83. */
#if MCU_ID == 407 || MCU_ID == 446
#define EX81_RAM_TOO_SMALL  1
#endif

#if EN_NET && EN_USB && EN_USB_NCM && EN_NET_DHCP && \
    EN_DHCP_SERVICE && EN_UDP_SERVER && EN_IP_DEFRAG && \
    !defined(EX81_RAM_TOO_SMALL)

/* ============================================================================
 * Constants
 * ============================================================================ */
#define UDP_SRV_PORT        7961U
#define MAX_CLIENTS         2U
#define CLIENT_TIMEOUT_MS   30000U
/* IP_DEFRAG_MAX_PAYLOAD defined in os_config.h (default 1480) */

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x81U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xC1U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
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
    s_ip         = ip;
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
    (void)dns;
    ca_arp_send_garp();
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * UDP server callbacks
 * ============================================================================ */
static void on_connect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] cid=%u  %u.%u.%u.%u:%u connected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_disconnect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] cid=%u  %u.%u.%u.%u:%u disconnected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_rx(u8 cid, ca_ipv4_t ip, u16 port, const u8 *pld, u16 len)
{
    /* Hardware CRC32: unlike a byte sum it also catches reordered or swapped
     * bytes, which is exactly what has to be proven when reassembling IP
     * fragments. Runs in the UDP worker task (never in ISR), so the mutex
     * inside ca_crc32_get() is safe here. */
    u32 csum = ca_crc32_get((u8 *)pld, (u32)len);

    ca_stdout_printf_n("UDP[7961] cid=%u  rx=%u B  crc32=0x%08X  from %u.%u.%u.%u:%u",
        (u32)cid, (u32)len, csum,
        ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);

    if (len >= 8U) {
        ca_stdout_printf_n("  pld[0..7]= %02X %02X %02X %02X %02X %02X %02X %02X",
            pld[0], pld[1], pld[2], pld[3],
            pld[4], pld[5], pld[6], pld[7]);
    }

    /* Full payload echo — analyzable with tcpdump for byte-by-byte verification */
    ca_udp_server_send(cid, pld, len);
}

static ca_udp_server_cfg_t srv_cfg = {
    .port              = UDP_SRV_PORT,
    .max_clients       = MAX_CLIENTS,
    .client_timeout_ms = CLIENT_TIMEOUT_MS,
    .local_mac         = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x81U } },
    .local_ip          = { .addr = 0U },
    .on_rx             = on_rx,
    .on_connect        = on_connect,
    .on_disconnect     = on_disconnect,
};

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x81U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX81: DHCP + UDP server + IP defrag over USB-NCM ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],
        s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("defrag: sessions=%u timeout=%ums payload_max=%uB",
        (u32)IP_DEFRAG_MAX_SESSIONS,
        (u32)IP_DEFRAG_TIMEOUT_MS,
        (u32)IP_DEFRAG_MAX_PAYLOAD);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex48) */
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

    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    /* Binding a port puts nothing on the wire, so it stays before the gate. */
    EVAL_UDP_SRV(ca_udp_server_bind();)

    /* The DORA must not start before the host has brought the interface up:
     * the NCM link goes up only when the host selects alt setting 1, and until
     * then ca_usb_ncm_send_frame() drops every frame and returns false -- a
     * DISCOVER sent too early is LOST, not delayed. But the wait CANNOT sit
     * before the loop: enumeration needs this task to be already servicing
     * frames, so blocking here would hold off the very event being waited for.
     *
     * The UDP server needs no gate of its own: it only ever answers. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            EVAL_DHCP_SVC(ca_dhcp_service_net_ready();)
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
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex48
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
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

/* Stats/pool diagnostics on request via CLI "udp stats" | "net": the "eth"
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

    /* Hardware CRC32 used by on_rx() to verify reassembled payloads */
    EVAL_CRC(ca_crc32_init();)

    ca_led_init();
    ca_led_off(1U);

    /* ca_net_init() is NOT called here (unlike ex48): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */

    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })
    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)

    /* IP defrag: allocates IP_DEFRAG_MAX_PAYLOAD × IP_DEFRAG_MAX_SESSIONS from heap */
    EVAL_IP_DEFRAG(
        if (!ca_ip_defrag_init(IP_DEFRAG_MAX_PAYLOAD))
            CA_SAFE_FAIL("ca_ip_defrag_init");
    )
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)
    EVAL_UDP_SRV(ca_udp_server_start(&srv_cfg);)
    EVAL_IP_DEFRAG(ca_ip_defrag_service_start();)
    EVAL_PACKET_TRACER(
        ca_pktrace_set_sender(STDOUT);
        ca_pktrace_start();
        ca_pktrace_enable(true);
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
PRIVATE_STRING(m81a, "=== Example 81: IP defrag + DHCP + UDP server over USB-NCM ===");
#ifdef EX81_RAM_TOO_SMALL
PRIVATE_STRING(m81b, "128K of RAM is not enough: IP defrag needs 11.6 KB of heap (8 x 1480), about 3 KB are free");
#else
PRIVATE_STRING(m81b, "EN_NET / EN_USB / EN_USB_NCM / EN_UDP_SERVER / EN_IP_DEFRAG == 0: no tasks created");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m81a);
    PUT_N(&priv_m81b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_IP_DEFRAG && ... */
