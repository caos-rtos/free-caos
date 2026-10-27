// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex83.c
 * @brief          : Example 83 — SSH server over USB-NCM
 ******************************************************************************
 * @details
 * NCM variant of example_67 (same service on ETH). Minimal SSH server
 * (curve25519-sha256 + ssh-ed25519 + aes256-gcm) over the caOS stack, with the
 * transport moved from the RMII PHY to USB-NCM. ex67 carries the NCM bring-up
 * commented out precisely as the base for this example.
 *
 * At startup:
 *   1) offline self-test of the binary packet protocol (RFC 4253 §6);
 *   2) bring-up USB-NCM + net core (ARP/ICMP);
 *   3) ca_tcp_service_start() + ca_ssh_service_init(22) → listen SSH.
 *
 * STATIC IP, no DHCP -- as in ex67. That removes the usual DHCP gate of the
 * NCM clones, but NOT the need to wait for the link: ca_ssh_service_init()
 * spawns the SSH task, which calls ca_tcp_listen() with
 * ca_net_proto_get_local_ip() (ssh_transport.c:1367). With a static address
 * that value is already valid, so the listen itself is safe early -- but the
 * first SYN-ACK still has to reach a host that has selected alt setting 1.
 * The service is therefore started only once the link is seen up, from inside
 * the loop, for the same reason as every other clone: blocking before the loop
 * would stall enumeration itself.
 *
 *   ssh test     re-executes the BPP framing vectors (offline)
 *
 * PC procedure (the NCM iface appears as enx02ca0500... or usb0):
 *   sudo ip addr add 192.168.10.1/24 dev <ncm-iface>
 *   sudo ip link set <ncm-iface> up
 *   ./examples/example_83/run_ssh_client_ncm.sh
 *
 * The script wraps the full command (test keypair in examples/caos_ed25519/):
 *   ssh -i examples/caos_ed25519/id_ed25519 -o IdentitiesOnly=yes \
 *       -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no \
 *       -v 192.168.10.83 -p 22
 * IdentitiesOnly stops ssh from offering the agent keys first; the host-key
 * options are required because the board regenerates its host key at each boot.
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
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
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */
#define SSH_TASK_STACK STACK_LARGE

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U
#define SSH_TASK_STACK (STACK_XLARGE+STACK_MEDIUM)   /* F4xx: extra headroom */

#elif MCU_ID == 563
#define USB_NUM      0U    /* USB_DRD_FS — single instance H563 */
#define SSH_TASK_STACK STACK_LARGE

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#define SSH_TASK_STACK STACK_LARGE
#endif /* MCU_ID */

/* RAM budget — parts with 128 KB (F407, F446)
 *
 * The .heap must return to the end of RAM, so the data below it has to fit:
 * here it does not, and the link fails with "cannot move location counter
 * backwards" rather than with a clear message about memory.
 *
 * This example turns on every service at once plus SSH and overflows by
 * ~22.8 KB — the widest of the NCM clones by far. Trimming NET_BUF_COUNT or
 * TCP_MAX_CONN does not come close; the part simply has 128 KB, of which
 * 26 KB are already the two per-EP USB queues in usb_fs_f4xx.c.
 * Measured on both boards, byte for byte the same. */
#if MCU_ID == 407 || MCU_ID == 446
#define EX83_RAM_TOO_SMALL  1
#endif

#if EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET && EN_USB && EN_USB_NCM && \
    !defined(EX83_RAM_TOO_SMALL)

/* ── Network identity — static IP, as in ex67 ────────────────────────────── */
static const ca_ipv4_t s_ip = { .v = { 192U, 168U, 10U, 83U } };

static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 83U } },
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x83U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xC3U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ── Net task: USB-NCM + stack + TCP + SSH listen ─────────────────────────── */
static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(500);

    ca_stdout_printf_n("\n\n=== EX83: SSH server over USB-NCM ===");
    ca_stdout_printf_n("IP  %u.%u.%u.%u  port %u",
        s_ip.v[0], s_ip.v[1], s_ip.v[2], s_ip.v[3], (u32)SSH_LISTEN_PORT);

#if EN_SSH_TEST
    /* Framing sanity before opening the port. */
    EVAL_CLI_SSH( ca_ssh_test_init(); )
    (void)ca_ssh_bpp_selftest();
#endif

    /* USB NCM bring-up (replaces eth_gpio_init() of ex67) */
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

    /* Net core + protocols (ARP/ICMP). Static IP: it is valid from here on,
     * which is what the SSH listen below needs. */
    ca_net_proto_init(&net_cfg);

    /* The ARP service, for its cache and its "arp" CLI node: net_proto
     * answers ARP on its own, so the wire works without this and the gap
     * stayed invisible until the bench compared EN_CLI_ARP against the
     * help. Static IP, so it is passed in directly. */
    ca_arp_service_init(net_cfg.mac, s_ip);


    /* TCP service can start right away: it puts nothing on the wire until
     * someone connects. */
    ca_tcp_service_start();

    /* The SSH service is started only once the host has brought the interface
     * up. There is no DHCP here to act as the gate (static IP), but the reason
     * for waiting is the same as in every other clone: until the host selects
     * alt setting 1 the NCM link is down and ca_usb_ncm_send_frame() drops
     * every frame -- an early SYN-ACK would be lost, not delayed.
     *
     * And, as always, the wait CANNOT sit before the loop: enumeration needs
     * this task to be already servicing frames, so blocking here would hold off
     * the very event being waited for. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool ssh_started = false;
    while (1) {
        if (!ssh_started && ca_usb_ncm_is_up()) {
            ssh_started = true;
            ca_stdout_put_n("USB NCM UP");
            if (!ca_ssh_service_init(SSH_LISTEN_PORT)) {
                ca_stdout_printf_n("[SSH] service init FAILED");
                return 1U;
            }
            ca_stdout_printf_n(
                "Set the PC iface to 192.168.10.1/24, then: ssh -v %u.%u.%u.%u",
                s_ip.v[0], s_ip.v[1], s_ip.v[2], s_ip.v[3]);
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

/* ── NCM link monitor — the NCM counterpart of task_eth_mon in ex67 ───────── */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            prev = up;
        }
        /* Static IP: nothing to renew when the link returns, unlike the DHCP
         * clones. The SSH service keeps its listen PCB across link bounces. */
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
        ca_cli_register(&s_dbg_cli);
    )

    ca_led_init();
    ca_led_off(1U);

    /* NB: no banner here: ca_os_configure() runs before ca_os_run(), with no
     * current task -> ca_stdout_* would print "Invalid call without Context
     * TLB=NULL". The banner is printed by task_net after startup.
     *
     * ca_net_init() is NOT called here either (unlike ex67 on ETH): the NCM
     * iface only exists after USB enumeration, so it lives in task_net. */
}

void ca_os_main(void)
{
    ca_os_job("net",     CA_TH_LEVEL4, task_net,     NULL,
              SSH_TASK_STACK, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon", CA_TH_LEVEL3, task_ncm_mon, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* incomplete configuration */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m83a, "=== Example 83: SSH server over USB-NCM ===");
#ifdef EX83_RAM_TOO_SMALL
PRIVATE_STRING(m83b, "DISABLED - 128K of RAM is not enough for every service plus SSH: short by ~22.8 KB");
#else
PRIVATE_STRING(m83b, "DISABLED - requires EN_SSH_SERVICE / EN_TCP_SERVICE / EN_NET / EN_USB / EN_USB_NCM");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m83a);
    PUT_N(&priv_m83b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_SSH_SERVICE && EN_TCP_SERVICE && EN_NET && EN_USB && EN_USB_NCM */
