// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex142.c
 * @brief          : Example 142 — example 141 (LVGL over VNC) over USB-NCM
 *                   Full Speed instead of Ethernet (H753, U575, H563, ...)
 ******************************************************************************
 * @details
 * NCM variant of example 141, as ex69-83 are of the Ethernet examples: the
 * application (LVGL screen, VNC service with its X509Plain login, DMA2D) is
 * the same code; only the interface handed to ca_net_init() differs. The
 * board appears on the PC as a USB network interface (CDC-NCM, 12 Mbit/s
 * bus, ~5-7 Mbit/s measured) and takes its address by DHCP from the PC
 * (tools/bench_net.sh usb).
 *
 * Why it is worth it: the VNC sends only what changed (fb dirty map), an LVGL
 * screen in use is ~0.3 MB/s, idle ~7 KB/s - inside Full Speed. It brings the
 * VNC to parts without Ethernet (U575: USB FS, DMA2D, AES).
 *
 *   e142 text | clear | stop | start   as e142
 *   vnc info                           the service (EN_CLI_VNC)
 *
 * Viewer: vncviewer <ip> (TigerVNC: X509Plain, user caos, password
 * caos-vnc-demo, certificate examples/example_139/tls_identity_cert.pem).
 *
 * REQUIREMENTS os_config.h: as example 141 with EN_ETH 0, EN_USB 1,
 * EN_USB_NCM 1, EN_USB_CDC 0.
 ******************************************************************************
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#if EN_VNC_TLS
#include "../example_139/tls_identity.h"
#include "../example_139/vnc_cred.h"
#endif

/* FREE edition (heap capped at half, 19/11): framebuffer + LVGL pool (32 KB)
 * + draw buffer + VNC buffers at the 320x200 lvgl_check.py needs do not fit
 * the F767 (175 KB), H563 (164 KB) or H773 (121 KB): not built there. */
#define E142_FREE_NO_ROOM  (CAOS_FREE && (MCU_ID == 767 || MCU_ID == 563 || MCU_ID == 773))

/* USB instance of the Full Speed port (as ex69-83) */
#if MCU_ID == 753
#define USB_NUM      1U          /* OTG_FS, CN13 */
#elif MCU_ID == 575 || MCU_ID == 563 || MCU_ID == 767 || MCU_ID == 773
#define USB_NUM      0U
#else
#undef  EN_USB
#define EN_USB       0
#endif

#if EN_USB && EN_USB_NCM && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && \
    EN_TCP_SERVICE && EN_FB && EN_VNC_SERVICE && MCU_ID != 439 && \
    !E142_FREE_NO_ROOM

#include "lvgl.h"

#if CAOS_FREE && MCU_ID == 753
#define SCR_W   320             /* FREE, 244 KB heap: 150 KB + LVGL + VNC ~215 KB */
#define SCR_H   240
#elif MCU_ID == 753 || MCU_ID == 747
#define SCR_W   480             /* 255 KB */
#define SCR_H   272
#elif MCU_ID == 773 || MCU_ID == 767
#define SCR_W   320             /* 125 KB. H773: at 320x240 the 232 KB heap had no
                                   room left for the TLS context (~25 KB). F767: fits
                                   since the USB queues follow the classes (26/11,
                                   NCM 13 KB instead of 43.5 KB) */
#define SCR_H   200
#else
#define SCR_W   320             /* 150 KB: F746, H563 (plain RFB, no TLS context) */
#define SCR_H   240
#endif


static const ca_mac_t s_mac = CA_EXAMPLE_MAC(142);
static ca_semaphore_t s_dhcp_sem;
static ca_fb_t s_fb = CA_FB_NONE;

/* USB NCM function; the host-side MAC */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = CA_MAC_BYTES(CA_MAC_ROLE_DEV2, 142),
};

/* ============================================================================
 * The screen (GUI task only)
 * ============================================================================ */
static lv_obj_t *s_ta;
static lv_obj_t *s_clicks;
static u32       s_click_n;

static void btn_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    s_click_n++;
    lv_label_set_text_fmt(s_clicks, "Clicks: %u", (unsigned)s_click_n);
    ca_stdout_printf_n("E142 button clicked %u", s_click_n);
}

static void toggle_cb(lv_event_t *e)
{
    lv_obj_t *t = lv_event_get_target_obj(e);
    ca_stdout_printf_n("E142 toggle %s", lv_obj_has_state(t, LV_STATE_CHECKED) ? "on" : "off");
}

static void ta_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED)
        ca_stdout_printf_n("E142 text \"%s\"", lv_textarea_get_text(s_ta));
    else if (code == LV_EVENT_READY)
        ca_stdout_printf_n("E142 ready \"%s\"", lv_textarea_get_text(s_ta));
}

static void build_ui(void *arg)
{
    (void)arg;
    lv_obj_t *scr = lv_screen_active();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "caOS + LVGL over VNC");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    /* lv_example_keyboard_textarea: created first, so it has the keys */
    s_ta = lv_textarea_create(scr);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_width(s_ta, lv_pct(90));
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_placeholder_text(s_ta, "Type here...");
    lv_obj_add_event_cb(s_ta, ta_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_ta, ta_cb, LV_EVENT_READY, NULL);

    /* lv_example_button_event, plus a toggle */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, SCR_W / 20, 62);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Click me");
    lv_obj_center(bl);

    s_clicks = lv_label_create(scr);
    lv_label_set_text(s_clicks, "Clicks: 0");
    lv_obj_align_to(s_clicks, btn, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_obj_t *tg = lv_button_create(scr);
    lv_obj_set_checkable(tg, true);
    lv_obj_align(tg, LV_ALIGN_TOP_RIGHT, -(SCR_W / 20), 62);
    lv_obj_add_event_cb(tg, toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *tl = lv_label_create(tg);
    lv_label_set_text(tl, "Toggle");
    lv_obj_center(tl);

    lv_obj_t *kb = lv_keyboard_create(scr);
    lv_obj_set_size(kb, lv_pct(100), lv_pct(45));
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_ta);

    ca_stdout_printf_n("E142 ready: LVGL %u.%u.%u, screen %ux%u RGB565, heap free %u",
                       (u32)LVGL_VERSION_MAJOR, (u32)LVGL_VERSION_MINOR, (u32)LVGL_VERSION_PATCH,
                       (u32)SCR_W, (u32)SCR_H, ca_memory_get_avail_memory());
}

/* ============================================================================
 * VNC -> LVGL
 * ============================================================================ */
static void on_key(ca_vnc_src_t src, bool down, u32 keysym)
{
    (void)src;
    ca_stdout_printf_n("E142 vnc key down=%u sym=%08X", (u32)down, keysym);
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
    .mac  = CA_EXAMPLE_MAC(142),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

/* USB first, then the net core on the NCM iface (it exists only after the
 * device is up), then DHCP once the host has brought the interface up: a
 * DISCOVER sent earlier is dropped (ex76 explains the gate in the loop). */
/* 'e142 start' runs in the CLI's cli_cmd task, whose STACK_LARGE stack would
 * take the place the GUI task has just left (H773: the PSP arena has room for
 * one of the two). The command only asks: the net task starts LVGL once
 * that cli_cmd has ended. */
static bool lvgl_start(void);
static volatile bool s_start_req;
static ca_task_handle_t s_start_by = CA_TASK_HANDLE_NONE;

static bool task_alive(ca_task_handle_t h)
{
    return h != CA_TASK_HANDLE_NONE &&
           ca_os_task_handle(ca_os_context_from_slot(h & 0xFFFFU)) == h;
}

static void lvgl_start_poll(void)
{
    if (!s_start_req || task_alive(s_start_by)) return;
    s_start_req = false;
    ca_stdout_printf_n("E142 lvgl start: %s", lvgl_start() ? "ok" : "FAILED");
}

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("\n\n=== EX142: LVGL over VNC over USB-NCM ===");
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
        lvgl_start_poll();
    }
    return 0U;
}

/* VNC once the board has its address; LVGL right away (it draws into the
 * framebuffer whether anyone looks or not) */
static u32 task_app(void *arg)
{
    (void)arg;
    ca_sem_wait(&s_dhcp_sem);
    const ca_vnc_cfg_t cfg = {
        .fb = s_fb, .port = 0U, .name = "caOS ex142 LVGL NCM",
        .security = ca_aes_is_hw() ? CA_VNC_SECURITY_AUTO : CA_VNC_SECURITY_NONE,
#if EN_VNC_TLS
        .cert_der = TLS_IDENTITY_CERT, .cert_len = sizeof(TLS_IDENTITY_CERT),
        .private_key = TLS_IDENTITY_KEY,
        .cred = &VNC_CRED,              /* X509Plain login: caos / caos-vnc-demo */
#endif
        .client_input = true, .on_key = on_key, .on_pointer = on_pointer,
    };
    const bool ok = ca_vnc_service_init(&cfg) && ca_vnc_service_start();
    /* start() only creates the task: its buffers (and the TLS context) are
     * taken there, and a missing one ends it. Look before saying listening. */
    ca_vnc_stats_t st;
    for (u32 i = 0U; i < 20U; i++) {
        ca_vnc_service_stats(&st);
        if (!ok || st.running || st.last_close != NULL) break;
        ca_os_sleep(100U);
    }
    if (ok && st.running)
        ca_stdout_printf_n("VNC listening on port %u, %s", (u32)VNC_DEFAULT_PORT,
                           st.tls ? "TLS" : "no encryption, input on");
    else
        ca_stdout_printf_n("VNC FAILED: %s", st.last_close != NULL ? st.last_close : "init");
    return 0U;
}

/* ============================================================================
 * CLI
 * ============================================================================ */
#if ENABLE_CLI
static void print_text(void *arg)
{
    (void)arg;
    ca_stdout_printf_n("E142 text area \"%s\"", lv_textarea_get_text(s_ta));
}

static void clear_text(void *arg)
{
    (void)arg;
    lv_textarea_set_text(s_ta, "");
    ca_stdout_put_n("E142 text area cleared");
}

static bool lvgl_start(void);

/* e142 — the line arrives without the command word */
static bool e142_cmd(c_str *args)
{
    const char *p = (*args)->data;
    /* The CLI task never touches LVGL: it hands the work to the GUI task */
    if (ca_strncmp(p, "text", 4) == 0) {
        if (!ca_lvgl_call(print_text, NULL)) CLI_PUT_N("E142 call queue full");
    } else if (ca_strncmp(p, "clear", 5) == 0) {
        if (!ca_lvgl_call(clear_text, NULL)) CLI_PUT_N("E142 call queue full");
    } else if (ca_strncmp(p, "stop", 4) == 0) {
        const bool ok = ca_lvgl_stop();
        CLI_PRINTF_N("E142 lvgl stop: %s, heap free %u", ok ? "ok" : "FAILED",
                     ca_memory_get_avail_memory());
    } else if (ca_strncmp(p, "start", 5) == 0) {
        const ca_task_handle_t me = ca_os_self_handle();
        const ca_thread_context_t *c = ca_os_context_from_slot(me & 0xFFFFU);
        if (c != NULL && ca_strncmp(c->name, "cli_cmd", 7) == 0) {
            s_start_by = me;            /* started by the net task once we end */
            s_start_req = true;
        } else {
            CLI_PRINTF_N("E142 lvgl start: %s", lvgl_start() ? "ok" : "FAILED");
        }
    } else {
        return false;                   /* cli prints the help line */
    }
    return true;
}
static const ca_cli_cmd_t s_e142_cmds[] = {
    { "e142", "e142      : text | clear | stop | start", { e142_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_e142_table = {
    "e142", s_e142_cmds, (u16)(sizeof(s_e142_cmds) / sizeof(s_e142_cmds[0])), NULL
};
#endif

/* LVGL on the framebuffer: at boot and again after 'e142 stop' */
static bool lvgl_start(void)
{
    const ca_lvgl_cfg_t lc = { .fb = s_fb, .buf_lines = 20U, .extra_heap = 0U,
                               .use_dma2d = true, .cursor = true,
                               .setup = build_ui, .setup_arg = NULL };
    s_click_n = 0U;
    return ca_lvgl_start(&lc);
}

/* ============================================================================
 * caOS hooks
 * ============================================================================ */
PRIVATE_STRING(nofb, "ex142: no memory for the framebuffer");
PRIVATE_STRING(nogui, "ex142: LVGL did not start");

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
        ca_cli_register_table(&s_e142_table);
    )
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    s_fb = ca_fb_create(SCR_W, SCR_H, CA_FB_FMT_RGB565, 1U);
    if (s_fb == CA_FB_NONE) PUT_N(&priv_nofb);

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
    if (s_fb != CA_FB_NONE && !lvgl_start()) PUT_N(&priv_nogui);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m142a, "=== Example 142: LVGL over VNC ===");
#if E142_FREE_NO_ROOM
PRIVATE_STRING(m142b, "DISABLED in the FREE edition on this board - framebuffer + LVGL + VNC do not fit half the heap (see LIMITS.md; LVGL alone: ex140)");
#elif MCU_ID == 439
/* 97 KB of heap: framebuffer + LVGL pool + draw buffer + TLS context need
 * ~150 KB (17/11/2026). The F439 keeps LVGL for small screens: ex140. */
PRIVATE_STRING(m142b, "DISABLED on F439 - not enough RAM for framebuffer + LVGL + TLS (heap 97 KB, needs ~150 KB)");
#else
PRIVATE_STRING(m142b, "DISABLED - needs USB NCM (EN_USB, EN_USB_NCM, EN_NET, DHCP, TCP), EN_FB and EN_VNC_SERVICE");
#endif
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m142a);
    PUT_N(&priv_m142b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
