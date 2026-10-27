// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 94 -- caOS Net Stack on TWO interfaces: ETH + USB-NCM together
 *
 * Step F0 of the ETH + NCM series (ex94-ex97, app_3). One stack, one MAC, one IP, two links:
 *   iface 0  eth0  ETH (RMII, LAN8742A)   pull, zero-copy, HW checksum
 *   iface 1  usb0  USB CDC-NCM            push, copy,      SW checksum
 *
 * What it proves (no bridging yet - frames are NOT forwarded between links):
 *   - a frame is tagged with the iface it came in on;
 *   - ARP replies and ICMP echo replies leave from THAT iface, so the board
 *     answers on both links with the same address;
 *   - a reply leaving from usb0 carries checksums computed in software, while
 *     the same reply on eth0 leaves them to the MAC (CIC);
 *   - 'ip if' shows both ifaces with their own rx/tx counters.
 *
 * Hardware: NUCLEO-H753ZI (OTG_FS, USB_NUM=1), NUCLEO-H563ZI (USB_DRD,
 *           USB_NUM=0), NUCLEO-F767ZI (OTG_FS, USB_NUM=0).
 *           Parts without ETH (U575, F446) report UNSUPPORTED.
 *
 * PC procedure (ETH holds 192.168.10.1, the NCM interface is up with no
 * address on the subnet - only a link-local /32, since rp_filter drops what
 * arrives on an interface with none - so the kernel cannot route around it):
 *   sudo ./tools/bench_net.sh up                 # eth: 192.168.10.1/24
 *   ping -c 3 192.168.10.94                       # leaves on eth
 *   ping -c 3 -I <ncm iface> 192.168.10.94        # forced out of usb
 *   board CLI: ip if                              # both counters moved
 *
 * REQUIREMENTS os_config.h: EN_ETH 1, EN_NET 1, EN_USB 1, EN_USB_NCM 1
 */

#include "caos.h"
#include "net_proto.h"

/* ── USB instance per part ────────────────────────────────────────────── */
#if MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS (as ex55) */
#elif MCU_ID == 563 || MCU_ID == 767
#define USB_NUM      0U    /* H563 USB_DRD_FS / F767 OTG_FS */
#endif

#if EN_ETH && EN_NET && EN_USB && EN_USB_NCM && defined(USB_NUM)

#define ETH_AF         11U

/* ============================================================================
 * Network identity: ONE address for both links
 * ============================================================================ */
static const ca_mac_t  s_mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x94U } };
static const ca_ipv4_t s_ip  = { .v = { 192U, 168U, 10U, 94U } };

static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 94U } },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x94U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x94U } },
    .link_cb  = on_link_change,
    /* rx_cb / error_cb: overwritten by ca_eth_iface_get() */
};

/* NCM: the MAC here is the HOST side one (what the PC gives its interface) */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x94U },
    /* .on_rx set by ca_ncm_iface_get() */
};

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
        ca_stdout_printf_n("Net task restarted -- init kept");
        goto rx_loop;
    }
    ca_os_sleep(300);

    ca_stdout_printf_n("\n\n=== EX94: caOS net stack on ETH + USB-NCM ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("IP   %u.%u.%u.%u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3]);

    /* Second interface: USB-NCM, registered next to eth0 (iface 0, the
     * default route for traffic the board starts itself) */
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

    ca_net_proto_init(&net_cfg);   /* also registers 'ping'/'ip' (EN_CLI_NET) */

    for (u8 i = 0U; i < ca_net_iface_count(); i++) {
        const ca_net_iface_t *f = ca_net_iface_at(i);
        ca_stdout_printf_n("iface %u %s csum=%s", (u32)f->id, f->name,
                           f->csum_offload ? "hw" : "sw");
    }

    ca_os_sleep(500);
    ca_eth_link_poll();
    ca_arp_send_garp();
    ca_os_sleep(50);

    ca_stdout_printf_n("Net task ready -- ping 192.168.10.94 on eth0 and usb0");

    s_net_up = true;

rx_loop:
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0;
}

/* ============================================================================
 * Link callback / monitor
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
    )

    ca_led_init();
    ca_led_off(1U);
    ca_led_off(3U);

    /* iface 0: ETH. usb0 joins from the net task, once USB is up. */
    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg))) {
        CA_SAFE_FAIL("ca_net_init");
    }
    ca_arp_service_init(s_mac, s_ip);
}

void ca_os_main(void)
{
    /* The task that sends the ARP requests: without it ca_arp_resolve()
     * only ever finds what the cache learned from incoming traffic. */
    ca_arp_service_start();
    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
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
PRIVATE_STRING(m94a, "=== Example 94 ===");
PRIVATE_STRING(m94b, "needs ETH and USB device (EN_ETH / EN_NET / EN_USB / EN_USB_NCM)");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m94a);
    PUT_N(&priv_m94b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
