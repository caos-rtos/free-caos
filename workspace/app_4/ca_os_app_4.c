/*
 * app_4 - an image for the caOS bootloader (workspace/app_2), H563 and H753.
 *
 * Built for the slots (image.cmake): <mcu>_app_4_slotA/_slotB.elf, then
 * tools/caos_image.py -> .cimg. It shows the image side of the contract
 * (caos/inc/boot_image.h):
 *
 *   trial    started as a trial, it takes the IWDG over (ca_boot_image_start)
 *            and confirms itself once the network is up (DHCP lease): at the
 *            next reset the bootloader keeps this slot
 *   update   once confirmed it listens for a .cimg pushed over TFTP
 *            (tools/caos_tftp put <ip> <file.cimg>), writes it into the other
 *            slot and resets into the bootloader, which starts it as a trial
 *
 * CLI: img info | img confirm.
 * Same MAC as the bootloader: the board keeps one DHCP address.
 *
 * Config: os_config_app_4.h (caos_config.py, never by hand).
 */
#include "caos.h"
#include "net_proto.h"

#if CA_BOOT_SUPPORTED

#define ETH_AF  11U

static const ca_mac_t s_mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0xB0U, 0x07U } };
static volatile bool  s_net_up = false;

/* =============================================================================
 * Network: DHCP, ARP, TFTP
 * ============================================================================= */
static void on_link_change(bool up)
{
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static ca_eth_cfg_t s_eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0xB0U, 0x07U } },
    .link_cb  = on_link_change,
};

static const ca_net_cfg_t s_net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0xB0U, 0x07U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b = ca_net_buf_ptr(buf_idx);
    u8  *frame  = b->data + b->data_off;
    u16  l4_off = b->meta.rx.l4_off;
    ca_dhcp_service_rx_handler((const ca_udp_frame_t *)(frame + l4_off),
                               (u16)(b->data_len - l4_off));
    ca_net_buf_free(buf_idx);
}

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    (void)mask; (void)gw; (void)dns;
    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_arp_send_garp();
    ca_stdout_printf_n("APP4: ip %u.%u.%u.%u", ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    s_net_up = true;
}

static void on_dhcp_expire(void)
{
    s_net_up = false;
}

static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
#if MCU_ID == 563
        { CA_GPIO_PORT_B, 15U },     /* RMII_TXD1: PB15 on NUCLEO-H563ZI (UM3115) */
#else
        { CA_GPIO_PORT_B, 13U },     /* PB13 on NUCLEO-H753ZI */
#endif
    };
    for (u32 i = 0U; i < sizeof(pins) / sizeof(pins[0]); i++)
        ca_gpio_configure(pins[i].port, pins[i].pin, CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
                          CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, ETH_AF);
}

static u32 task_net(void *arg)
{
    (void)arg;
    ca_net_proto_init(&s_net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    ca_eth_link_poll();
    ca_os_sleep(200);
    ca_dhcp_service_net_ready();
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) { ca_eth_link_poll(); ca_os_sleep(500); }
    return 0U;
}

/* =============================================================================
 * Image: who am I, confirm, update
 * ============================================================================= */
/* The header the bootloader checked sits right before the vector table */
static void print_self(void)
{
    const char me = ca_boot_image_slot();
    if (me == 0) {
        ca_stdout_put_n("APP4: not started from a slot");
        return;
    }
    const u8 *hdr = (const u8 *)(SCB->VTOR - CA_BOOT_HDR_SIZE);
    u32 version;
    ca_memcpy(&version, hdr + 16U, 4U);
    ca_stdout_printf_n("APP4: slot %c v%u \"%s\"%s", me, version, (const char *)(hdr + 20U),
                       ca_boot_is_trial() ? " - TRIAL, IWDG taken over" : "");
}

static u32 task_app(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    print_self();

    /* Healthy = the network came up: confirm (does nothing if not a trial) */
    while (!s_net_up) ca_os_sleep(200);
    if (ca_boot_is_trial()) {
        ca_boot_confirm();
        ca_stdout_put_n("APP4: confirmed - the bootloader keeps this slot");
    }

    /* Update from the running image: one .cimg into the other slot */
    ca_stdout_put_n("APP4: update - tools/caos_tftp put <ip> <image.cimg>");
    while (1) {
        (void)ca_boot_receive_update(s_mac, CA_WAIT_FOREVER);   /* resets on success */
        ca_os_sleep(500);
    }
    return 0U;
}

static bool img_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("img       : info | confirm  (image started by the bootloader)");
        return false;
    }
    if (ca_strncmp(p, "img", 3) != 0) return false;
    p = ca_skip_sp(p + 3);
    if (ca_strncmp(p, "confirm", 7) == 0) {
        ca_boot_confirm();
        CLI_PUT_N(ca_boot_is_trial() ? "img: confirmed" : "img: not a trial, nothing to confirm");
    } else {
        const u32 st = ca_bkp_read(CA_BOOT_BKP_STATE);
        CLI_PRINTF_N("img: slot %c, trial %s, state 0x%08X",
                     ca_boot_image_slot() ? ca_boot_image_slot() : '-',
                     ca_boot_is_trial() ? "yes" : "no", st);
    }
    return true;
}

static ca_cli_node_t s_img_node = { img_cli, NULL };

void ca_os_configure(void)
{
    ca_led_init();
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_img_node);
    )
    ca_flash_init();
    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&s_eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");
    ca_ipv4_t zero = { .addr = 0U };
    ca_arp_service_init(s_mac, zero);
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
    ca_tftp_service_init(s_mac);
}

void ca_os_main(void)
{
    ca_boot_image_start();
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_os_job("net", CA_TH_LEVEL4, task_net, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("app", CA_TH_LEVEL3, task_app, NULL, STACK_MEDIUM, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else /* !CA_BOOT_SUPPORTED */

void ca_os_configure(void)
{
    /* The bootloader slots exist only on the H563 and H753 */
    CA_EXAMPLE_UNSUPPORTED();
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif
