// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 69 -- caOS UDP server service on port 7961, over USB-NCM
 *
 * NCM variant of example_34 (same UDP server on ETH). The application logic is
 * identical: what changes is only the network interface handed to the net core.
 * ca_ncm_iface_get() registers the ca_usb_ncm service as a PUSH interface, so
 * the UDP server, ARP and ICMP above it are the very same code as on ETH --
 * which is the point of the ca_net_iface_t abstraction.
 *
 *   ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM);
 *   ca_usb_dev_mng_add(&s_ncm.header);
 *   ca_usb_dev_mng_start();
 *   ca_net_init(ca_ncm_iface_get(&s_ncm));   // iface push (was ca_eth_iface_get)
 *   ca_net_proto_init(&net_cfg);             // ARP/ICMP
 *
 * There is no PHY here: no eth_gpio_init(), no ca_eth_link_poll(), no link
 * monitor task. The link is the USB enumeration, reported by ca_usb_ncm_is_up().
 *
 * Functionality:
 *   1. UDP server listening on port 7961 via ca_udp_server_start()
 *   2. Each new client gets a dedicated worker thread (internal to the service)
 *   3. Echo of received data with prefix "ECHO: "
 *   4. Automatic disconnection after CLIENT_TIMEOUT_MS ms of inactivity
 *   5. Maximum MAX_CLIENTS simultaneous clients
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * Test from PC (X = interface usb0 / enxNN):
 *   sudo ip addr add 192.168.10.1/24 dev usb0
 *   sudo ip link set usb0 up
 *   echo "hello" | nc -u 192.168.10.69 7961
 *   nc -u 192.168.10.69 7961      # interactive
 *   or the bench:  ./hw_test.sh -n ex69   (test/hw_test/src/test_ex69.cpp)
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 | EN_UDP_SERVER 1
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


#if EN_NET && EN_USB && EN_USB_NCM && EN_UDP_SERVER

/* ============================================================================
 * Constants
 * ============================================================================ */
#define UDP_SRV_PORT       7961U
#define MAX_CLIENTS        4U
#define CLIENT_TIMEOUT_MS  10000U

/* ============================================================================
 * Network identity
 * ============================================================================ */
static const ca_mac_t  s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x69U } };
static const ca_ipv4_t s_ip  = { .v = { 192U, 168U, 10U, 69U } };

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xA9U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * UDP server callbacks
 * ============================================================================ */

static void on_connect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] client %u: %u.%u.%u.%u:%u connected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_disconnect(u8 cid, ca_ipv4_t ip, u16 port)
{
    ca_stdout_printf_n("UDP[7961] client %u: %u.%u.%u.%u:%u disconnected",
        (u32)cid, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);
}

static void on_rx(u8 cid, ca_ipv4_t ip, u16 port,
                  const u8 *pld, u16 len)
{
    char txt[64U];
    u16 tlen = len < (u16)(sizeof(txt) - 1U) ? len : (u16)(sizeof(txt) - 1U);
    for (u16 i = 0U; i < tlen; i++) {
        u8 c = pld[i];
        txt[i] = (c >= 0x20U && c < 0x7FU) ? (char)c : '.';
    }
    txt[tlen] = '\0';
    ca_stdout_printf_n("UDP[7961] client %u: rx %uB ('%s') from %u.%u.%u.%u:%u",
        (u32)cid, (u32)len, txt, ip.v[0], ip.v[1], ip.v[2], ip.v[3], (u32)port);

    u8 reply[256U];
    static const u8 pfx[] = "ECHO: ";
    const u16 pfxlen = (u16)(sizeof(pfx) - 1U);
    u16 rlen = pfxlen + len;
    if (rlen > (u16)sizeof(reply)) rlen = (u16)sizeof(reply);
    for (u16 i = 0U; i < pfxlen; i++) reply[i] = pfx[i];
    u16 dlen = rlen - pfxlen;
    for (u16 i = 0U; i < dlen; i++) reply[pfxlen + i] = pld[i];
    ca_udp_server_send(cid, reply, rlen);
}

static const ca_udp_server_cfg_t srv_cfg = {
    .port              = UDP_SRV_PORT,
    .max_clients       = MAX_CLIENTS,
    .client_timeout_ms = CLIENT_TIMEOUT_MS,
    .local_mac         = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x69U } },
    .local_ip          = { .v = { 192U, 168U, 10U, 69U } },
    .on_rx             = on_rx,
    .on_connect        = on_connect,
    .on_disconnect     = on_disconnect,
};

/* ============================================================================
 * Net config + net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 69U } },
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x69U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_printf_n("\n\n=== EX69: UDP server port %u over USB-NCM ===",
        (u32)UDP_SRV_PORT);
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("IP   %u.%u.%u.%u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3]);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex34) */
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_printf_n("[USB] init FAILED"); return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_printf_n("[USB] add NCM FAILED - check EN_USB_NCM"); return 1U;
    }
    ca_usb_dev_mng_start();

    /* Attach NCM to the net core as push iface + protocols */
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_printf_n("[NET] ca_net_init FAILED"); return 1U;
    }
    ca_net_proto_init(&net_cfg);
    EVAL_UDP_SRV(ca_udp_server_bind();)

    /* No ca_eth_link_poll()/GARP here: the link is the USB enumeration, and the
     * host learns our MAC from the NCM descriptors, not from a gratuitous ARP.
     *
     * And NO wait on ca_usb_ncm_is_up() before the loop below: the link goes up
     * only once the host has enumerated us and selected alt setting 1, and the
     * stack needs this task to be already servicing frames for that to happen.
     * Blocking here would hold off the very event being waited for. This server
     * is reactive anyway -- it answers, it never speaks first. */
    ca_stdout_printf_n("UDP server ready - bring up usb0, then: nc -u %u.%u.%u.%u %u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3], (u32)UDP_SRV_PORT);

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

/* No "eth stats"/"eth pool" here: that CLI lives inside #if EN_ETH in
 * caos/drivers/src/eth.c, so it is not built on NCM (EN_CLI_ETH is off).
 * Pool and net counters stay reachable through "net" (EN_CLI_NET) and the
 * "usbdbg" handler below. */

/* ============================================================================
 * Heartbeat
 * ============================================================================ */
static u32 task_heartbeat(void *arg)
{
    (void)arg;
    ca_led_init();
    while (1) {
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

    /* ca_net_init() is NOT called here (unlike ex34): the NCM iface only exists
     * after the USB device is up, so both live in task_net. */
    EVAL_ARP_SVC(ca_arp_service_init(s_mac, s_ip);)
}

void ca_os_main(void)
{
    EVAL_ARP_SVC(ca_arp_service_start();)
    EVAL_UDP_SRV(ca_udp_server_start(&srv_cfg);)
    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m69a, "=== Example 69 ===");
PRIVATE_STRING(m69b, "EN_NET / EN_USB / EN_USB_NCM / EN_UDP_SERVER == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m69a);
    PUT_N(&priv_m69b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_UDP_SERVER */
