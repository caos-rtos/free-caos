// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex158.c
 * @brief          : Example 158 — over USB-NCM: LVGL widgets built from MicroPython (caos.ui)
 ******************************************************************************
 * @details
 * USB-NCM clone of example 156 (as ex142 is of ex141): the same application,
 * the network on the board's USB Full Speed port instead of Ethernet; the PC
 * gives the address by DHCP on its USB interface (tools/bench_net.sh usb).
 * For the parts whose Ethernet is not cabled on the bench (H563).
 *
 * LVGL on an RGB565 framebuffer served by VNC, as example 141, and the
 * widgets come from Python (upy_lvgl.md):
 *
 *  - the C program starts LVGL (ca_lvgl_start) with a screen of its own -
 *    one label, "ex158 app screen" - and calls ca_python_ui_enable();
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
 *   e158 info     LVGL running, heap free
 *   vnc info | start | stop | kick   (EN_CLI_VNC)
 ******************************************************************************
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"

/* USB instance of the Full Speed port (as ex69-83, ex142) */
#if MCU_ID == 753
#define USB_NUM      1U          /* OTG_FS, CN13 */
#elif MCU_ID == 575 || MCU_ID == 563 || MCU_ID == 767 || MCU_ID == 773
#define USB_NUM      0U
#else
#undef  EN_USB
#define EN_USB       0
#endif

#if EN_USB && EN_USB_NCM && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && \
    EN_TCP_SERVICE && EN_FB && EN_VNC_SERVICE && EN_PYTHON && ENABLE_CLI && \
    (MCU_ID == 753 || MCU_ID == 563)

#include "lvgl.h"

#define FB_WIDTH    320
#define FB_HEIGHT   240

static const ca_mac_t s_mac = CA_EXAMPLE_MAC(158);
static ca_semaphore_t s_dhcp_sem;
static ca_fb_t        s_fb = CA_FB_NONE;
static bool           s_gui = false;

/* USB NCM function; the host-side MAC */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = CA_MAC_BYTES(CA_MAC_ROLE_DEV2, 158),
};

/* ============================================================================
 * LVGL: the application's screen, input from the viewer
 * ============================================================================ */
/* in the GUI task, once */
static void build_app_screen(void *arg)
{
    (void)arg;
    lv_obj_t *l = lv_label_create(lv_screen_active());
    lv_label_set_text(l, "ex158 app screen");
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
    ca_sem_post(&s_dhcp_sem);
}

static void on_dhcp_expire(void)
{
    ca_stdout_put_n("DHCP lease EXPIRED");
}

static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = CA_EXAMPLE_MAC(158),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

/* USB first, then the net core on the NCM iface (it exists only after the
 * device is up), then DHCP once the host has brought the interface up: a
 * DISCOVER sent earlier is dropped (as ex142). */
static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("\n\n=== EX158: LVGL widgets from MicroPython, over VNC over USB-NCM ===");
    ca_stdout_printf_n("E158 fb %ux%u RGB565, LVGL %s, heap free %u", (u32)FB_WIDTH,
                       (u32)FB_HEIGHT, s_gui ? "running, caos.ui enabled" : "NOT running",
                       ca_memory_get_avail_memory());
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) { ca_stdout_put_n("[USB] init FAILED"); return 1U; }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) { ca_stdout_put_n("[USB] add NCM FAILED"); return 1U; }
    ca_usb_dev_mng_start();
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) { ca_stdout_put_n("[NET] ca_net_init FAILED"); return 1U; }
    ca_net_proto_init(&net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    ca_stdout_put_n("waiting for the host to bring the interface up...");
    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            ca_dhcp_service_net_ready();
            ca_stdout_put_n("DHCP: starting DORA...");
        }
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) ca_net_process();
    }
    return 0U;
}

/* VNC starts once the board has its address: the listen socket takes it */
static u32 task_app(void *arg)
{
    (void)arg;
    ca_sem_wait(&s_dhcp_sem);
    const ca_vnc_cfg_t cfg = {
        .fb = s_fb, .port = 0U, .name = "caOS ex158 (caos.ui)",
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
static bool e158_cmd(c_str *args)
{
    const char *p = (*args)->data;
    while (*p == ' ') p++;
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("E158 fb %ux%u RGB565, LVGL %s, heap free %u", (u32)FB_WIDTH, (u32)FB_HEIGHT,
                     s_gui ? "running" : "NOT running", ca_memory_get_avail_memory());
        return true;
    }
    return false;                   /* cli prints the help line */
}

static const ca_cli_cmd_t s_e158_cmds[] = {
    { "e158", "e158      : info (LVGL, heap)", { e158_cmd, (ca_stack_size_t)0 } },
};

static ca_cli_table_t s_e158_table = {
    "e158", s_e158_cmds, (u16)(sizeof(s_e158_cmds) / sizeof(s_e158_cmds[0])), NULL
};

/* ============================================================================
 * caOS hooks
 * ============================================================================ */
PRIVATE_STRING(nofb, "ex158: no memory for the framebuffer");
PRIVATE_STRING(nogui, "ex158: LVGL did not start");

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
        ca_cli_register_table(&s_e158_table);
    )
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    s_fb = ca_fb_create(FB_WIDTH, FB_HEIGHT, CA_FB_FMT_RGB565, 1U);
    if (s_fb == CA_FB_NONE) PUT_N(&priv_nofb);
    ca_python_ui_enable();          /* caos.ui on the LVGL started below */

    /* ca_net_init() is in task_net: the NCM iface exists after USB is up */
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
    ca_os_job("net",     CA_TH_LEVEL4, task_net,     NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
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

PRIVATE_STRING(m158a, "=== Example 158: LVGL widgets from MicroPython ===");
PRIVATE_STRING(m158b, "needs USB-NCM + VNC + EN_PYTHON + EN_FB and the RAM of H753 or H563 (RGB565 fb + LVGL + VNC + Python heap)");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m158a);
    PUT_N(&priv_m158b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
