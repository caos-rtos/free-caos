// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex156.c
 * @brief          : Example 156 — LVGL widgets built from MicroPython (caos.ui)
 ******************************************************************************
 * @details
 * LVGL on an RGB565 framebuffer served by VNC, as example 141, and the
 * widgets come from Python (upy_lvgl.md):
 *
 *  - the C program starts LVGL (ca_lvgl_start) with a screen of its own -
 *    one label, "ex156 app screen" - and calls ca_python_ui_enable();
 *  - a script builds on Python's own screen with caos.ui; at the end of
 *    the VM its widgets go and the application's screen comes back;
 *  - the viewer's pointer and keys go to LVGL (ca_lvgl_input_*), the widget
 *    events to Python (ui.event, ui.run, o.on(fn)).
 *
 *   >>> from caos import ui
 *   >>> b = ui.Button(None, 'Start'); b.size(120, 40); b.align(ui.CENTER)
 *   >>> b.on(lambda o, e, v: print('clicked')); ui.run(10000)
 *
 * Plain RFB with client input (as example 155): on a trusted LAN only.
 *
 * MEMORY (heap of the elf, KB): framebuffer + LVGL pool 32 + draw buffer
 * (20 lines) + VNC buffers + MicroPython heap (UPY_HEAP_SIZE 128 KB, halved
 * by the master when it is not there):
 *   H753 476: 320x240 (150) + 32 + 13 + 17 + 128 -> ~130 free
 *   H563 316: 320x240 (150) + 32 + 13 + 17 -> 128 do not fit, Python 64, ~40 free
 * H773 and F767 left out: with the RGB565 framebuffer even 64 KB of Python
 * do not fit next to LVGL and VNC; F439 smaller still.
 *
 * CLI:
 *   py            MicroPython REPL (Ctrl-D on an empty line exits)
 *   e156 info     LVGL running, heap free
 *   vnc info | start | stop | kick   (EN_CLI_VNC)
 ******************************************************************************
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"

#if EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && \
    EN_TCP_SERVICE && EN_FB && EN_VNC_SERVICE && EN_PYTHON && ENABLE_CLI && \
    (MCU_ID == 753 || MCU_ID == 563)

#include "lvgl.h"

#define FB_WIDTH    320
#define FB_HEIGHT   240
#define ETH_AF      11U

static const ca_mac_t s_mac = CA_EXAMPLE_MAC(156);
static ca_semaphore_t s_dhcp_sem;
static ca_fb_t        s_fb = CA_FB_NONE;
static bool           s_gui = false;

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = CA_EXAMPLE_MAC(156),
    .link_cb  = on_link_change,
};

/* ============================================================================
 * LVGL: the application's screen, input from the viewer
 * ============================================================================ */
/* in the GUI task, once */
static void build_app_screen(void *arg)
{
    (void)arg;
    lv_obj_t *l = lv_label_create(lv_screen_active());
    lv_label_set_text(l, "ex156 app screen");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
}

static void on_key(ca_vnc_src_t src, bool down, u32 keysym)
{
    (void)src;
    (void)ca_lvgl_input_key(down, keysym);
}

static void on_pointer(ca_vnc_src_t src, u8 buttons, u16 x, u16 y)
{
    (void)src;
    ca_lvgl_input_pointer(buttons, x, y);
}

/* ============================================================================
 * Network: DHCP, ARP, net task (as ex139)
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
    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u", ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
    ca_arp_send_garp();
    ca_sem_post(&s_dhcp_sem);
}

static void on_dhcp_expire(void)
{
    ca_stdout_put_n("DHCP lease EXPIRED");
}

static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = CA_EXAMPLE_MAC(156),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("\n\n=== EX156: LVGL widgets from MicroPython, over VNC ===");
    ca_stdout_printf_n("E156 fb %ux%u RGB565, LVGL %s, heap free %u", (u32)FB_WIDTH,
                       (u32)FB_HEIGHT, s_gui ? "running, caos.ui enabled" : "NOT running",
                       ca_memory_get_avail_memory());
    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    ca_eth_link_poll();
    ca_os_sleep(200);
    ca_dhcp_service_net_ready();
    ca_stdout_put_n("DHCP: starting DORA...");
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) {
        ca_eth_link_poll();
        ca_led_toggle(3U);
        ca_os_sleep(500);
    }
    return 0U;
}

/* VNC starts once the board has its address: the listen socket takes it */
static u32 task_app(void *arg)
{
    (void)arg;
    ca_sem_wait(&s_dhcp_sem);
    const ca_vnc_cfg_t cfg = {
        .fb = s_fb, .port = 0U, .name = "caOS ex156 (caos.ui)",
        .security = CA_VNC_SECURITY_NONE,       /* plain RFB, input on: the app's choice */
        .client_input = true, .on_key = on_key, .on_pointer = on_pointer,
    };
    const bool ok = s_fb != CA_FB_NONE && ca_vnc_service_init(&cfg) && ca_vnc_service_start();
    ca_stdout_printf_n("VNC %s on port %u, no encryption", ok ? "listening" : "FAILED",
                       (u32)VNC_DEFAULT_PORT);
    return 0U;
}

/* ============================================================================
 * CLI
 * ============================================================================ */
/* Immediate: one line, runs in cli */
static bool e156_cmd(c_str *args)
{
    const char *p = (*args)->data;
    while (*p == ' ') p++;
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("E156 fb %ux%u RGB565, LVGL %s, heap free %u", (u32)FB_WIDTH, (u32)FB_HEIGHT,
                     s_gui ? "running" : "NOT running", ca_memory_get_avail_memory());
        return true;
    }
    return false;                   /* cli prints the help line */
}

static const ca_cli_cmd_t s_e156_cmds[] = {
    { "e156", "e156      : info (LVGL, heap)", { e156_cmd, (ca_stack_size_t)0 } },
};

static ca_cli_table_t s_e156_table = {
    "e156", s_e156_cmds, (u16)(sizeof(s_e156_cmds) / sizeof(s_e156_cmds[0])), NULL
};

/* ============================================================================
 * GPIO ETH (as ex139)
 * ============================================================================ */
typedef struct { u8 port; u8 pin; } e156_pin_t;

static void eth_gpio_init(void)
{
#if MCU_ID == 773
    static const e156_pin_t pins[] = {
        { CA_GPIO_PORT_B,  6U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_G,  6U },
        { CA_GPIO_PORT_A,  7U }, { CA_GPIO_PORT_G,  4U }, { CA_GPIO_PORT_G,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U }, { CA_GPIO_PORT_G, 12U },
    };
#else
    static const e156_pin_t pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
#if MCU_ID == 563
        { CA_GPIO_PORT_B, 15U },        /* RMII_TXD1 on NUCLEO-H563ZI */
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
PRIVATE_STRING(nofb, "ex156: no memory for the framebuffer");
PRIVATE_STRING(nogui, "ex156: LVGL did not start");

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
        ca_cli_register_table(&s_e156_table);
    )
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    s_fb = ca_fb_create(FB_WIDTH, FB_HEIGHT, CA_FB_FMT_RGB565, 1U);
    if (s_fb == CA_FB_NONE) PUT_N(&priv_nofb);
    ca_python_ui_enable();          /* caos.ui on the LVGL started below */

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");
    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    }
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
    ca_sem_init(&s_dhcp_sem, "DHCPBND", 0U, 1U);
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();
    ca_os_job("net",     CA_TH_LEVEL4, task_net,     NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("app",     CA_TH_LEVEL3, task_app,     NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    if (s_fb != CA_FB_NONE) {
        const ca_lvgl_cfg_t lc = { .fb = s_fb, .buf_lines = 20U, .extra_heap = 0U,
                                   .use_dma2d = true, .cursor = true,
                                   .setup = build_app_screen, .setup_arg = NULL };
        s_gui = ca_lvgl_start(&lc);
        if (!s_gui) PUT_N(&priv_nogui);
    }
    ca_os_run(true);
}

#else

PRIVATE_STRING(m156a, "=== Example 156: LVGL widgets from MicroPython ===");
PRIVATE_STRING(m156b, "needs Ethernet + VNC + EN_PYTHON + EN_FB and the RAM of H753 or H563 (RGB565 fb + LVGL + VNC + Python heap)");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m156a);
    PUT_N(&priv_m156b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
