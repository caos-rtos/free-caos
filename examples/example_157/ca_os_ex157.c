// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex157.c
 * @brief          : Example 157 — over USB-NCM: MicroPython draws, a VNC viewer sees it
 ******************************************************************************
 * @details
 * USB-NCM clone of example 155 (as ex142 is of ex141): the same application,
 * the network on the board's USB Full Speed port instead of Ethernet; the PC
 * gives the address by DHCP on its USB interface (tools/bench_net.sh usb).
 * For the parts whose Ethernet is not cabled on the bench (H563).
 * Memory over NCM on the H563 (27/11, USB queues sized by class): heap 298 KB,
 * Python gets its 128 KB, 53 KB free with the VM up.
 *
 * The VNC service of example 139 on a framebuffer that Python draws in:
 *
 *  - the C program makes an L8 framebuffer (palette below), serves it with
 *    VNC on port 5900 and publishes it to Python as caos.FB('vnc')
 *    (ca_python_publish_fb): whatever a script draws, the viewer shows;
 *  - the viewer's keys and pointer go to the C callbacks, which put them in
 *    the queue 'vncin' published to Python (ca_python_publish_queue, 8-byte
 *    items): a script reads them with caos.Queue('vncin');
 *  - framebuf.GS8 is the framebuffer's own format (one byte per pixel, an
 *    index of the palette): a script draws a block with framebuf and blits it.
 *
 *   >>> import caos, framebuf
 *   >>> c = caos.FB('vnc'); c.fill(0); c.fill_rect(10, 10, 100, 40, 2)
 *   >>> q = caos.Queue('vncin'); q.recv(5000)
 *
 *   palette   0 black, 1 white, 2..7 red green blue yellow cyan magenta,
 *             8..255 a gray ramp (i, i, i)
 *   vncin     b'K' down 0 0 keysym(u32 LE)  |  b'P' buttons x(u16 LE) y(u16 LE) 0 0
 *
 * Plain RFB with client input (CA_VNC_SECURITY_NONE, the application's
 * choice): the TLS context and its crypto would take the RAM Python needs.
 * Use it on a trusted LAN only.
 *
 * MEMORY: framebuffer + MicroPython heap (UPY_HEAP_SIZE 128 KB: graphics
 * scripts keep framebuf bytearrays and objects) + VNC buffers (~27 KB at 640
 * px wide, the conversion lines are most of it) on the heap left after
 * .data/.bss. The master halves its heap when 128 KB are not there. Built
 * only where it fits (heap of the ex157 elf, KB, 27/11/2026):
 *   H753 476: 640x480 (300) + VNC 27 + Python 128 -> 18 KB free (measured)
 *   H563 316: 400x300 (117) + 22 + 128 -> ~46 KB free (480x320: 13)
 *   H773 231: 320x200 (63)  + 17 + 128 -> ~20 KB free (320x240: 8)
 *   F767 210: 320x240 (75)  + 17 -> 128 do not fit, Python gets 64, ~51 free
 * F439 (100 KB of heap) is left out: Python and VNC leave no room for a
 * framebuffer worth showing. No Ethernet: F446, U575.
 *
 * CLI:
 *   py                MicroPython REPL (Ctrl-D on an empty line exits)
 *   e157 info         framebuffer size, events queued and dropped, heap free
 *   vnc info | start | stop | kick   (EN_CLI_VNC)
 *
 * PC side: examples/example_155/vnc_py_check.py (raw RFB client, the script
 * Python runs and the image it must produce come from the same table).
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
    (MCU_ID == 753 || MCU_ID == 563 || MCU_ID == 773 || MCU_ID == 767)

#if MCU_ID == 753
#define FB_WIDTH  640
#define FB_HEIGHT 480
#elif MCU_ID == 563
#define FB_WIDTH  400           /* 480x320 left 13 KB with 128 KB of Python */
#define FB_HEIGHT 300
#elif MCU_ID == 773
#define FB_WIDTH  320           /* 320x240 left 8 KB with 128 KB of Python */
#define FB_HEIGHT 200
#else
#define FB_WIDTH  320           /* F767: 128 KB do not fit, the master takes 64 */
#define FB_HEIGHT 240
#endif

#define EV_ITEM     8U          /* bytes of one 'vncin' item */
#define EV_DEPTH    32U

static const ca_mac_t s_mac = CA_EXAMPLE_MAC(157);
static ca_semaphore_t s_dhcp_sem;
static ca_fb_t        s_fb = CA_FB_NONE;
static ca_queue_t     s_ev;
static bool           s_ev_ok = false;
static u32            s_ev_sent, s_ev_dropped;

/* USB NCM function; the host-side MAC */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = CA_MAC_BYTES(CA_MAC_ROLE_DEV2, 157),
};

/* ============================================================================
 * Palette and input
 * ============================================================================ */
static const u32 s_colors[8] = {
    0x000000U, 0xFFFFFFU, 0xFF0000U, 0x00FF00U, 0x0000FFU, 0xFFFF00U, 0x00FFFFU, 0xFF00FFU
};

static void set_palette(void)
{
    (void)ca_fb_set_palette(s_fb, 0U, 8U, s_colors);
    for (u32 i = 8U; i < 256U; i++) {
        const u32 g = i * 0x010101U;
        (void)ca_fb_set_palette(s_fb, (u16)i, 1U, &g);
    }
}

/* from the VNC task: never blocks, a full queue drops (and counts) */
static void ev_put(const u8 *item)
{
    if (s_ev_ok && ca_queue_heap_send(&s_ev, item, 0U)) s_ev_sent++;
    else s_ev_dropped++;
}

static void on_key(ca_vnc_src_t src, bool down, u32 keysym)
{
    (void)src;
    const u8 it[EV_ITEM] = { 'K', (u8)down, 0U, 0U, (u8)keysym, (u8)(keysym >> 8),
                             (u8)(keysym >> 16), (u8)(keysym >> 24) };
    ev_put(it);
    ca_stdout_printf_n("E157 key down=%u sym=%08X", (u32)down, keysym);
}

static void on_pointer(ca_vnc_src_t src, u8 buttons, u16 x, u16 y)
{
    (void)src;
    const u8 it[EV_ITEM] = { 'P', buttons, (u8)x, (u8)(x >> 8), (u8)y, (u8)(y >> 8), 0U, 0U };
    ev_put(it);
    ca_stdout_printf_n("E157 ptr btn=%02X x=%u y=%u", (u32)buttons, (u32)x, (u32)y);
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
    .mac  = CA_EXAMPLE_MAC(157),
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
    ca_stdout_put_n("\n\n=== EX157: MicroPython draws, VNC shows over USB-NCM ===");
    ca_stdout_printf_n("E157 fb %ux%u L8 %s, heap free %u", (u32)FB_WIDTH, (u32)FB_HEIGHT,
                       s_fb != CA_FB_NONE ? "published as caos.FB('vnc')" : "MISSING",
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
        .fb = s_fb, .port = 0U, .name = "caOS ex157 (MicroPython)",
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
/* Immediate: a few counters, runs in cli */
static bool e157_cmd(c_str *args)
{
    const char *p = (*args)->data;
    while (*p == ' ') p++;
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("E157 fb %ux%u L8, events queued %u dropped %u waiting %u, heap free %u",
                     (u32)FB_WIDTH, (u32)FB_HEIGHT, s_ev_sent, s_ev_dropped,
                     s_ev_ok ? ca_queue_heap_count(&s_ev) : 0U, ca_memory_get_avail_memory());
        return true;
    }
    return false;                   /* cli prints the help line */
}

static const ca_cli_cmd_t s_e157_cmds[] = {
    { "e157", "e157      : info (framebuffer, VNC events for Python, heap)",
      { e157_cmd, (ca_stack_size_t)0 } },
};

static ca_cli_table_t s_e157_table = {
    "e157", s_e157_cmds, (u16)(sizeof(s_e157_cmds) / sizeof(s_e157_cmds[0])), NULL
};

/* ============================================================================
 * caOS hooks
 * ============================================================================ */
PRIVATE_STRING(nofb, "ex157: no memory for the framebuffer");
PRIVATE_STRING(noev, "ex157: no memory for the 'vncin' queue");

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
        ca_cli_register_table(&s_e157_table);
    )
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    s_fb = ca_fb_create(FB_WIDTH, FB_HEIGHT, CA_FB_FMT_L8, 1U);
    if (s_fb == CA_FB_NONE || !ca_python_publish_fb("vnc", s_fb)) {
        PUT_N(&priv_nofb);
    } else {
        set_palette();
        (void)ca_fb_clear(s_fb, 0U);
    }
    s_ev_ok = ca_queue_heap_init(&s_ev, EV_ITEM, EV_DEPTH)
              && ca_python_publish_queue("vncin", &s_ev, EV_ITEM);
    if (!s_ev_ok) PUT_N(&priv_noev);

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
    ca_os_run(true);
}

#else

PRIVATE_STRING(m157a, "=== Example 157: MicroPython draws, VNC shows ===");
PRIVATE_STRING(m157b, "needs USB-NCM + VNC + EN_PYTHON and the RAM of H753, H563, H773 or F767 (fb + VNC + Python heap)");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m157a);
    PUT_N(&priv_m157b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
