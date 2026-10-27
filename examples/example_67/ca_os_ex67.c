// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex67.c
 * @brief          : Example 67 — SSH server bring-up (phase 2: transport)
 ******************************************************************************
 * @details
 * Minimal SSH server (curve25519-sha256 + ssh-ed25519 + aes256-gcm) over
 * the caOS stack, with transport over Ethernet (RMII PHY).
 *
 * The USB-NCM bring-up this example started from is kept commented out below,
 * as the base for the equivalent NCM example.
 *
 * At startup:
 *   1) offline self-test of the binary packet protocol (RFC 4253 §6);
 *   2) bring-up ETH + net core (ARP/ICMP);
 *   3) ca_tcp_service_start() + ca_ssh_service_init(22) → listen SSH.
 *
 * Phase 2 (transport): the handshake reaches NEWKEYS. The per-state trace
 * (EN_SSH_TRACE) shows version-exchange → KEXINIT → KEX X25519 → NEWKEYS.
 * Auth / channel / shell are phase 3.
 *
 *   ssh test     re-executes the BPP framing vectors (offline)
 *
 * PC Procedure (X = the Ethernet interface wired to the board):
 *   sudo ip addr add 192.168.10.1/24 dev <eth-iface>
 *   sudo ip link set <eth-iface> up
 *   ./examples/example_67/run_ssh_client.sh        # board has a static IP
 *
 * The script wraps the full command (test keypair in examples/caos_ed25519/):
 *   ssh -i examples/caos_ed25519/id_ed25519 -o IdentitiesOnly=yes \
 *       -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
 *       -v 192.168.10.67 -p 22
 * IdentitiesOnly stops ssh from offering the agent keys first; the host-key
 * options are required because the board regenerates its host key at each boot.
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_ETH 1 |
 *                        EN_TCP_SERVICE 1 | EN_SSH_SERVICE 1 | EN_HASH 1
 ******************************************************************************
 */

#include "caos.h"

#if EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET
/* The ONE client allowed to log in: the Ed25519 public key of the bench's
 * demo pair. The application owns it (ssh_service.h) - this definition wins
 * over the library's weak default, so the example works the same built in
 * the repo or against the precompiled FREE libraries. For your own board:
 * ssh-keygen -t ed25519, and put the 32 bytes of YOUR public key here. */
const u8 ca_ssh_authorized_pub[ED25519_PUBLIC_SIZE] = {
    0x9B, 0xAE, 0xAA, 0x4F, 0xD2, 0x8C, 0xB8, 0x79,
    0x00, 0x8D, 0x92, 0x34, 0x69, 0x7C, 0xFC, 0x2E,
    0x5A, 0x6E, 0x95, 0x2E, 0x4D, 0xE7, 0x35, 0xDF,
    0x86, 0x4F, 0x42, 0xC1, 0xA5, 0x15, 0xC7, 0xDF };
#endif
#include "net_proto.h"

/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
#define USB_NUM      0U
#define SSH_TASK_STACK STACK_LARGE

#elif MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS CN14 */
#define SSH_TASK_STACK STACK_LARGE

#elif MCU_ID == 407
#define USB_NUM      0U
#define SSH_TASK_STACK (STACK_XLARGE+STACK_MEDIUM)   /* F4xx: extra headroom */

#elif MCU_ID == 563
#define USB_NUM      0U    /* USB_DRD_FS — single instance H563 */
#define SSH_TASK_STACK STACK_LARGE

#elif MCU_ID == 767
#define USB_NUM      0U    /* OTG_FS CN13 */
#define SSH_TASK_STACK STACK_LARGE

#else
/* example not supported by hardware (no Ethernet PHY on this board) */
#undef EN_ETH
#define EN_ETH 0
#define SSH_TASK_STACK STACK_LARGE
#endif /* MCU_ID */

#if EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET && EN_ETH

/* ── Network identity ────────────────────────────────────────────────────── */
static const ca_ipv4_t s_ip = { .v = { 192U, 168U, 10U, 67U } };

static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 67U } },
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x67U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

#define ETH_AF  11U

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x67U } },
    .link_cb  = on_link_change,
};

/* Static IP, so there is no DHCP bind to confirm the cable is fine: report the
 * PHY link state instead. The board's only LED is taken by the heartbeat. */
static void on_link_change(bool up)
{
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
        /* RMII_TXD1: NUCLEO-H563ZI (UM3115) uses PB15 (JP6 ON); H753ZI uses PB13 */
#if MCU_ID == 563
        { CA_GPIO_PORT_B, 15U },
#else
        { CA_GPIO_PORT_B, 13U },
#endif /* EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET && EN_ETH */
    };
    for (u32 i = 0U; i < sizeof(pins)/sizeof(pins[0]); i++) {
        ca_gpio_configure(pins[i].port, pins[i].pin,
            CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
            CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, ETH_AF);
    }
}

/* --- USB-NCM variant (kept for the equivalent NCM example) ---------------
 * static ca_usb_ncm_cfg_t s_ncm = {
 *     .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
 *     .mps    = 64U,
 *     .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xA7U },
 *     .on_rx is set by ca_ncm_iface_get()
 * };
 * ---------------------------------------------------------------------- */

/* ── Net task: ETH + stack + TCP + SSH listen ─────────────────────────────── */
static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(500);

    ca_stdout_printf_n("\n\n=== EX67: SSH server su ETH ===");
    ca_stdout_printf_n("IP  %u.%u.%u.%u  port 22",
        s_ip.v[0], s_ip.v[1], s_ip.v[2], s_ip.v[3]);

#if EN_SSH_TEST
    /* Framing sanity before opening the port. */
    EVAL_CLI_SSH( ca_ssh_test_init(); )
    (void)ca_ssh_bpp_selftest();
#endif /* EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET && EN_ETH */

    /* ETH bring-up (RMII PHY) */
    eth_gpio_init();

    /* --- USB-NCM variant (see the commented cfg above) -------------------
     * if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) { ... return 1U; }
     * if (!ca_usb_dev_mng_add(&s_ncm.header))          { ... return 1U; }
     * ca_usb_dev_mng_start();
     * if (!ca_net_init(ca_ncm_iface_get(&s_ncm)))      { ... return 1U; }
     * ------------------------------------------------------------------- */

    /* Net core + protocols (ARP/ICMP) */
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg))) { ca_stdout_printf_n("[NET] init FAILED"); return 1U; }
    ca_net_proto_init(&net_cfg);

    /* The ARP service, for its cache and its "arp" CLI node: net_proto
     * answers ARP on its own, so the wire works without this and the gap
     * stayed invisible until the bench compared EN_CLI_ARP against the
     * help. Static IP, so it is passed in directly. */
    ca_arp_service_init(net_cfg.mac, s_ip);

    /* NO PACKET TRACER HERE, DELIBERATELY.
     *
     * EN_PACKET_TRACER is 1 but nothing calls ca_pktrace_start(), so the
     * service is compiled in and never runs. That looks like an oversight and
     * it was tried as a fix on 15/09 - the board's own view of what it sends
     * would have answered the question the PC-side capture could not.
     *
     * It was taken out again the same day, on purpose. The tracer prints every
     * frame to a 115200 console, which is a delay of the same order as the
     * sleep, the tcpdump attach and EN_TCP_TRACE - all three of which made
     * ex67's race disappear by accident. Adding a fourth observer to a defect
     * that three observers have already hidden measures the observer.
     *
     * If the board's side of the wire is ever needed, turn the tracer on for a
     * deliberate investigation - never while deciding whether this test passes.
     */

    ca_eth_link_poll();
    ca_os_sleep(200);

    /* TCP service + SSH server */
    ca_tcp_service_start();
    if (!ca_ssh_service_init(SSH_LISTEN_PORT)) {
        ca_stdout_printf_n("[SSH] service init FAILED");
        return 1U;
    }

    ca_stdout_printf_n("Set the PC iface to 192.168.10.1/24, then: ssh -v %u.%u.%u.%u",
        s_ip.v[0], s_ip.v[1], s_ip.v[2], s_ip.v[3]);

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

/* ── ETH monitor ──────────────────────────────────────────────────────────── */
static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) {
        ca_eth_link_poll();
        ca_os_sleep(500);
    }
    return 0U;
}

/* ── Heartbeat ────────────────────────────────────────────────────────────── */
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
    /* NB: no banner here: ca_os_configure() runs before ca_os_run(),
     * with no current task -> ca_stdout_* would print "Invalid call without
     * Context TLB=NULL". The banner is printed by task_net after start-up. */
}

void ca_os_main(void)
{
    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              SSH_TASK_STACK, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* incomplete configuration */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m67a, "=== Example 67 ===");
PRIVATE_STRING(m67b, "DISABLED - requires EN_SSH_SERVICE / EN_TCP_SERVICE / EN_NET / EN_ETH");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m67a);
    PUT_N(&priv_m67b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET && EN_ETH */
