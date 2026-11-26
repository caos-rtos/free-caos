// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex150.c
 * @brief          : Example 150 — MicroPython and the caOS network over USB-NCM
 ******************************************************************************
 * @details
 * Example 149 on USB-NCM instead of Ethernet: the same services (DHCP, ARP,
 * ping, NTP, DNS, syslog, TCP, UDP client) and the MicroPython REPL, with
 * caos.net and the socket module. The Python side is identical: the port
 * talks to the services, the services to ca_net_iface_t, and only the
 * interface handed to ca_net_init() changes (example 72 is the same move for
 * example 37). For the boards without Ethernet (U575) this is how Python
 * reaches the network.
 *
 * What differs from example 149, as in every NCM clone (ex69-83):
 *  - the USB device and the NCM interface are brought up in task_net, and
 *    ca_net_init() is called there: the interface exists only after it;
 *  - no PHY, no link poll: the link is ca_usb_ncm_is_up(), and the DORA is
 *    released only once the host has brought the interface up;
 *  - ca_python_set_mac() gets the device-side MAC, the one the services use.
 *
 *   py             MicroPython REPL (Ctrl-D on an empty line exits)
 *   dns, ntp, syslog, dhcp, arp, tcp ...   the services' own commands
 *
 * PC side: the bench moves its subnet to the USB interface (bench_net.sh usb)
 * and serves DHCP, the resolver of example 72 and the syslog collector of
 * example 74 there.
 ******************************************************************************
 */

#include "caos.h"
#include "net_proto.h"

/* ── USB instance per MCU family (as example 72) ──────────────────────── */
#if MCU_ID == 575
#define USB_NUM      0U
#elif MCU_ID == 753
#define USB_NUM      1U          /* OTG_FS CN14 */
#elif MCU_ID == 407 || MCU_ID == 439 || MCU_ID == 767
#define USB_NUM      0U
/* Not the F446: 128 KB of RAM in all. The static queues of the USB driver
 * (~26 KB, every data endpoint), the net buffer pool and the rest no longer
 * fit (the link overflows by 2 KB), and Python would have no heap left.
 * MicroPython runs there without the network (examples 145-148). */
#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS */
#elif MCU_ID == 773
#define USB_NUM      0U          /* OTG_HS, integrated PHY run at FS */
#else
#undef EN_USB
#define EN_USB 0
#endif

#if EN_NET && EN_USB && EN_USB_NCM && EN_NET_DHCP && EN_NET_NTP && EN_NET_DNS && \
    EN_NET_SYSLOG && EN_DHCP_SERVICE && EN_NTP_SERVICE && EN_DNS_SERVICE && \
    EN_SYSLOG_SERVICE && EN_PYTHON

/* ============================================================================
 * Constants — on a point-to-point USB link the PC is gateway, DHCP server,
 * resolver, NTP server and syslog collector at once
 * ============================================================================ */
static const ca_ipv4_t NTP_SERVER_IP_FALLBACK = { .v = { 192U, 168U, 10U, 1U } };
static const ca_ipv4_t DNS_SERVER_IP_FALLBACK = { .v = { 192U, 168U, 10U, 1U } };
static const ca_ipv4_t SYSLOG_SERVER_IP       = { .v = { 192U, 168U, 10U, 1U } };

/* ============================================================================
 * Network — fixed MAC, IP from DHCP
 * ============================================================================ */
/* device-side MAC (NCM clones: 02:CA:05:00:00:<NN>), host-side differs */
static const ca_mac_t s_mac = CA_EXAMPLE_MAC(150);

static volatile ca_ipv4_t s_ip   = { .addr = 0U };
static volatile bool      s_dhcp_bound = false;

static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = CA_EXAMPLE_MAC_HOST(150),  /* host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * DHCP UDP handler (port 68)
 * ============================================================================ */
static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b = ca_net_buf_ptr(buf_idx);
    u8 *frame = b->data + b->data_off;
    u16 l4_off = b->meta.rx.l4_off;
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

    /* NTP: opt42 from the lease, fallback to the gateway */
    EVAL_NTP_SVC({
        ca_ipv4_t ntp_ip = ca_dhcp_service_get_ntp_server();
        if (ntp_ip.addr == 0U) ntp_ip = gw;
        ca_ntp_service_set_server(ntp_ip);
        ca_ntp_service_net_ready();
    })

    /* DNS: opt6 from the lease, fallback to the gateway */
    EVAL_DNS_SVC({
        ca_ipv4_t dns_ip = dns;
        if (dns_ip.addr == 0U) dns_ip = gw;
        ca_dns_service_set_server(dns_ip);
        ca_dns_service_net_ready();
    })

    /* syslog: the collector is the PC */
    ca_syslog_service_net_ready();
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED - renewing...");
}

/* ============================================================================
 * NTP callbacks
 * ============================================================================ */
static void on_ntp_sync(u32 unix_time)
{
    ca_stdout_printf_n("NTP SYNC  unix=%u", unix_time);
}

static void on_ntp_fail(void)
{
    ca_stdout_put_n("NTP FAIL  server unreachable");
}

/* ============================================================================
 * Net task — USB-NCM bring-up, then the frame loop
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = CA_EXAMPLE_MAC(150),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX150: MicroPython + DHCP + NTP + DNS + syslog over USB-NCM ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2], s_mac.v[3], s_mac.v[4], s_mac.v[5]);
    ca_stdout_put_n("IP   --- (waiting for DHCP)");

    /* USB NCM bring-up (replaces eth_gpio_init() of example 149) */
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_put_n("[USB] init FAILED"); return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_put_n("[USB] add NCM FAILED - check EN_USB_NCM"); return 1U;
    }
    ca_usb_dev_mng_start();

    /* the NCM interface exists only now: ca_net_init() here, not in
     * ca_os_configure() as on Ethernet */
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_put_n("[NET] ca_net_init FAILED"); return 1U;
    }
    ca_net_proto_init(&net_cfg);

    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    EVAL_NTP_SVC(ca_ntp_service_bind();)
    EVAL_DNS_SVC(ca_dns_service_bind();)

    /* The DORA waits for the host to bring the interface up: before that
     * every frame is dropped (example 72 explains why the gate lives inside
     * the loop - enumeration needs this task servicing frames). */
    ca_stdout_put_n("waiting for the host to bring the interface up...");
    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            EVAL_DHCP_SVC(ca_dhcp_service_net_ready();)
            ca_stdout_put_n("DHCP: starting DORA...");
        }
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) {
            ca_net_process();
        }
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor (the NCM counterpart of task_eth_mon in example 149)
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;
    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            if (up) ca_led_on(1U); else ca_led_off(1U);
            if (up && s_dhcp_bound) {
                ca_stdout_put_n("link back UP: requesting lease renew");
                EVAL_DHCP_SVC(ca_dhcp_service_renew();)
            }
            prev = up;
        }
        ca_os_sleep(500);
    }
    return 0U;
}

static u32 task_heartbeat(void *arg)
{
    (void)arg;
    while (1) { ca_led_toggle(3U); ca_os_sleep(500); }
    return 0U;
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

    ca_led_init();
    ca_led_off(1U);
    ca_led_off(3U);

    /* ca_net_init() is NOT called here: see task_net */
    EVAL_ARP_SVC({
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    })
    EVAL_DHCP_SVC(ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);)
    EVAL_NTP_SVC(ca_ntp_service_init(NTP_SERVER_IP_FALLBACK, s_mac, on_ntp_sync, on_ntp_fail);)
    EVAL_DNS_SVC(ca_dns_service_init(DNS_SERVER_IP_FALLBACK, s_mac);)
    ca_syslog_service_init(SYSLOG_SERVER_IP, s_mac, SYSLOG_FAC_LOCAL0);

    /* socket module: the device-side MAC of the services above; the address
     * comes from the lease (TCP with EN_TCP_SERVICE, UDP with EN_UDP_CLIENT) */
    ca_python_set_mac(s_mac);
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_DHCP_SVC(ca_dhcp_service_start();)
    EVAL_NTP_SVC(ca_ntp_service_start();)
    EVAL_DNS_SVC(ca_dns_service_start();)
    ca_syslog_service_start();
    EVAL_TCP_SVC(ca_tcp_service_start();)

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m150a, "=== Example 150 ===");
PRIVATE_STRING(m150b, "ex150: needs EN_USB_NCM, EN_NET, DHCP, NTP, DNS, syslog and EN_PYTHON (not F446: RAM)");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m150a);
    PUT_N(&priv_m150b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && ... && EN_PYTHON */
