// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 139 -- caOS VNC server on a framebuffer
 *
 * Hardware: a Nucleo with Ethernet (H753, F767, H563, F439, H773 ...)
 *
 * The board draws a test pattern into an L8 framebuffer (fb driver, sized
 * for the RAM of the part) and serves it with the VNC service on port 5900.
 * Any VNC viewer on the LAN shows it: `vncviewer <ip>` (TigerVNC), Remmina.
 *
 * Security CA_VNC_SECURITY_AUTO: where the part has AES in hardware (H753,
 * H773, F439) the session is VeNCrypt X509Plain, all of it over TLS 1.3
 * (AES-256-GCM-SHA384), identity tls_identity.h from tools/gen_tls_identity.py
 * (self-signed, CN caOS VNC, SAN caos.local; the viewer asks to accept it, or
 * trusts examples/example_139/tls_identity_cert.pem). The viewer asks for a
 * login: user caos, password caos-vnc-demo (vnc_cred.h, tools/gen_vnc_cred.py;
 * the board keeps only a PBKDF2 hash). Elsewhere plain RFB,
 * view only. 'e139 sec none|tls|auto' + 'e139 destroy' + 'e139 new' change it
 * (Renode has no CRYP and no RNG: its test runs 'sec none').
 *
 *   pattern   palette 0 black, 1 white, 2..7 red green blue yellow cyan
 *             magenta, the rest a gray ramp. Index of pixel (x, y):
 *               1            if x % 32 == 0, y % 32 == 0, x == W-1 or y == H-1
 *               2 + 6x / W   else if y < 32       (colour bars)
 *               0            otherwise
 *   input     client input on: a pointer press paints a 3x3 yellow (5)
 *             square at the pointer; every key and pointer event is printed
 *             with its source. 'e139 key' / 'e139 ptr' inject events as
 *             local hardware would (ca_vnc_inject_*), through the SAME
 *             callbacks.
 *
 *   e139 grid                   redraw the pattern
 *   e139 box <x> <y> <w> <h> <c>  fill a rectangle with index c
 *   e139 pal <i> <rrggbb>       one palette entry
 *   e139 crc                    FNV-1a 32 of the pixels
 *   e139 key <keysym hex>       inject a key press+release (source local)
 *   e139 ptr <x> <y> <buttons>  inject a pointer event (source local)
 *   e139 input on|off           client input on / view only (injection stays)
 *   e139 destroy                destroy the service: its task ends and the
 *                               framebuffer goes with it; prints the heap
 *   e139 new                    new framebuffer + pattern, service again
 *   e139 sec auto|tls|none      security used by the next 'e139 new'
 *   vnc info | start | stop | kick   the service (EN_CLI_VNC)
 *
 * Test from PC: examples/example_139/vnc_check.py <ip> (raw RFB client:
 * the pattern in three pixel formats, incremental updates, input, a second
 * client refused). Renode: simu/tests/net.robot, tag vnc.
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_ARP_SERVICE 1, EN_DHCP_SERVICE 1, EN_NET_DHCP 1,
 *   EN_TCP_SERVICE 1, EN_FB 1, EN_VNC_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#if EN_VNC_TLS
#include "tls_identity.h"
#include "vnc_cred.h"
#endif

#if EN_ETH && EN_NET && EN_ARP_SERVICE && EN_DHCP_SERVICE && EN_NET_DHCP && \
    EN_TCP_SERVICE && EN_FB && EN_VNC_SERVICE

#if CAOS_FREE && MCU_ID == 753
/* FREE edition: the heap is capped at half (244 KB, 19/11): 640x480 L8
 * (301 KB) does not fit; 480x320 + the VNC buffers take ~163 KB */
#define FB_WIDTH  480
#define FB_HEIGHT 320
#elif MCU_ID == 753 || MCU_ID == 747
#define FB_WIDTH  640
#define FB_HEIGHT 480
#elif MCU_ID == 563 || MCU_ID == 767 || MCU_ID == 746
#define FB_WIDTH  480
#define FB_HEIGHT 320
#elif MCU_ID == 773
#define FB_WIDTH  320
#define FB_HEIGHT 240
#else
#define FB_WIDTH  320           /* F439: 91 KB heap, the TLS context takes */
#define FB_HEIGHT 160           /* ~24.7 KB: 320x200 (64 KB) left no room */
#endif

#define ETH_AF         11U

static const ca_mac_t s_mac = CA_EXAMPLE_MAC(139);
static ca_semaphore_t s_dhcp_sem;
static ca_fb_t s_fb = CA_FB_NONE;
static ca_vnc_security_t s_sec = CA_VNC_SECURITY_AUTO;
/* Format of the next 'e139 new'. RGB565 at half the lines: the same bytes as
 * the L8 framebuffer of this part (640 x 480 RGB565 would fit no MCU). */
static ca_fb_fmt_t s_fmt = CA_FB_FMT_L8;
static i32 s_w = FB_WIDTH, s_h = FB_HEIGHT;
static u8  s_buf = 1U;                  /* buffers asked at the next 'e139 new' */

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = CA_EXAMPLE_MAC(139),
    .link_cb  = on_link_change,
};

/* ============================================================================
 * Pattern
 * ============================================================================ */
static const u32 s_colors[8] = {
    0x000000U, 0xFFFFFFU, 0xFF0000U, 0x00FF00U, 0x0000FFU, 0xFFFF00U, 0x00FFFFU, 0xFF00FFU
};

/* The colour of palette index i in the framebuffer's format: the index for
 * L8, the palette colour reduced to 5-6-5 for RGB565 */
static u32 px(u32 i)
{
    if (s_fmt == CA_FB_FMT_L8) return i;
    const u32 rgb = (i < 8U) ? s_colors[i] : i * 0x010101U;
    return (((rgb >> 19) & 0x1FU) << 11) | (((rgb >> 10) & 0x3FU) << 5) | ((rgb >> 3) & 0x1FU);
}

static void draw_pattern(void)
{
    (void)ca_fb_set_palette(s_fb, 0U, 8U, s_colors);
    for (u32 i = 8U; i < 256U; i++) {
        const u32 g = i * 0x010101U;
        (void)ca_fb_set_palette(s_fb, (u16)i, 1U, &g);
    }
    (void)ca_fb_clear(s_fb, px(0U));
    for (i32 x = 0; x < s_w; x++) {
        const u32 c = 2U + (u32)((6 * x) / s_w);
        (void)ca_fb_fill_rect(s_fb, x, 0, 1, 32, px(c));
    }
    for (i32 x = 0; x < s_w; x += 32) (void)ca_fb_fill_rect(s_fb, x, 0, 1, s_h, px(1U));
    for (i32 y = 0; y < s_h; y += 32) (void)ca_fb_fill_rect(s_fb, 0, y, s_w, 1, px(1U));
    (void)ca_fb_fill_rect(s_fb, s_w - 1, 0, 1, s_h, px(1U));
    (void)ca_fb_fill_rect(s_fb, 0, s_h - 1, s_w, 1, px(1U));
    (void)ca_fb_present(s_fb);          /* the pattern is one frame */
}

/* ============================================================================
 * Input: one path for every source
 * ============================================================================ */
static const char *src_name(ca_vnc_src_t s)
{
    return s == CA_VNC_SRC_CLIENT ? "client" : (s == CA_VNC_SRC_LOCAL ? "local" : "remote");
}

static void on_key(ca_vnc_src_t src, bool down, u32 keysym)
{
    ca_stdout_printf_n("E139 key src=%s down=%u sym=%08X", src_name(src), (u32)down, keysym);
}

static void on_pointer(ca_vnc_src_t src, u8 buttons, u16 x, u16 y)
{
    ca_stdout_printf_n("E139 ptr src=%s btn=%02X x=%u y=%u", src_name(src), (u32)buttons, (u32)x, (u32)y);
    if (buttons & 1U) {
        (void)ca_fb_fill_rect(s_fb, (i32)x - 1, (i32)y - 1, 3, 3, px(5U));
        (void)ca_fb_present(s_fb);      /* frame complete: a double buffer shows it */
    }
}

/* ============================================================================
 * Network: DHCP, ARP, net task (as ex137)
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
    .mac  = CA_EXAMPLE_MAC(139),
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("\n\n=== EX139: VNC server on a framebuffer ===");
    ca_stdout_printf_n("Framebuffer %ux%u L8, VNC port %u", (u32)FB_WIDTH, (u32)FB_HEIGHT,
                       (u32)VNC_DEFAULT_PORT);
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

static bool vnc_up(void)
{
    const ca_vnc_cfg_t cfg = {
        .fb = s_fb, .port = 0U, .name = "caOS ex139",
        .security = s_sec,
#if EN_VNC_TLS
        .cert_der = TLS_IDENTITY_CERT, .cert_len = sizeof(TLS_IDENTITY_CERT),
        .private_key = TLS_IDENTITY_KEY,
        .cred = &VNC_CRED,              /* X509Plain login: caos / caos-vnc-demo */
#endif
        .client_input = true, .on_key = on_key, .on_pointer = on_pointer,
    };
    return ca_vnc_service_init(&cfg) && ca_vnc_service_start();
}

/* VNC starts once the board has its address: the listen socket takes it */
static u32 task_app(void *arg)
{
    (void)arg;
    ca_sem_wait(&s_dhcp_sem);
    const bool ok = vnc_up();
    ca_vnc_stats_t st;
    ca_vnc_service_stats(&st);
    ca_stdout_printf_n("VNC %s on port %u, %s", ok ? "listening" : "FAILED", (u32)VNC_DEFAULT_PORT,
                       st.tls ? "TLS" : "no encryption");
    return 0U;
}

/* ============================================================================
 * CLI
 * ============================================================================ */
#if ENABLE_CLI
static i32 arg(const char **p, u16 base)
{
    const char *s = ca_skip_sp(*p);
    const i32 v = ca_strtol(s, base);
    while (*s != '\0' && *s != ' ') s++;
    *p = s;
    return v;
}

#if EN_DMA2D && defined(DMA2D)
/* 'e139 conv [lines]' (vnc_add.md D11): one RGB565 frame to a 32 bpp client
 * pixel, the way vnc_proto.c does it (a LUT per channel, one line buffer),
 * against the DMA2D (PFC RGB565 -> ARGB8888, strips of `lines` lines, the
 * output buffer VNC would need). Conversion only: no TX, no TLS, no hash. */
#define E139_CONV_REPS  10U

static void e139_conv(u32 lines)
{
    ca_fb_info_t in;
    if (!ca_fb_info(s_fb, &in)) { CLI_PUT_N("E139 conv: no framebuffer"); return; }
    const bool l8 = in.fmt == CA_FB_FMT_L8;
    const u32 W = in.width, H = in.height;
    if (lines < 1U || lines > H) lines = 16U;
    u8 *buf = (u8 *)ca_memory_allocate(W * lines * 4U);
    if (buf == NULL) { CLI_PRINTF_N("E139 conv: no memory for %u lines", lines); return; }

    static const ca_vnc_pixfmt_t pf = { 32U, 24U, 0U, 1U, { 255U, 255U, 255U }, { 16U, 8U, 0U } };
    static u32 lut[256];
    if (l8) {
        for (u32 i = 0U; i < 256U; i++) lut[i] = ca_vnc_proto_pixel(&pf, in.palette[i]);
    } else {
        for (u32 i = 0U; i < 32U; i++) {
            const u32 c8 = (i << 3) | (i >> 2);
            lut[i]       = ca_vnc_proto_pixel(&pf, c8 << 16);
            lut[96U + i] = ca_vnc_proto_pixel(&pf, c8);
        }
        for (u32 i = 0U; i < 64U; i++)
            lut[32U + i] = ca_vnc_proto_pixel(&pf, ((i << 2) | (i >> 4)) << 8);
    }

    /* software: as rect() in vnc_proto.c, line by line */
    u32 t0 = ca_os_tick_ms();
    for (u32 r = 0U; r < E139_CONV_REPS; r++) {
        for (u32 row = 0U; row < H; row++) {
            const u8 *src = in.base + row * in.stride;
            u8 *o = buf;
            for (u32 i = 0U; i < W; i++) {
                u32 v;
                if (l8) {
                    v = lut[src[i]];
                } else {
                    const u32 c = (u32)src[2U * i] | ((u32)src[2U * i + 1U] << 8);
                    v = lut[c >> 11] | lut[32U + ((c >> 5) & 0x3FU)] | lut[96U + (c & 0x1FU)];
                }
                *o++ = (u8)v; *o++ = (u8)(v >> 8); *o++ = (u8)(v >> 16); *o++ = (u8)(v >> 24);
            }
        }
    }
    const u32 sw = ca_os_tick_ms() - t0;
    static u32 ref[640];                    /* last line, software */
    const u32 nref = (W < 640U) ? W : 640U;
    for (u32 i = 0U; i < nref; i++) ref[i] = ((const u32 *)buf)[i] & 0x00FFFFFFU;

    /* DMA2D: strips of `lines` lines */
    (void)ca_dma2d_init();
    bool ok = ca_dma2d_lock(1000U);             /* shared with the VNC service */
    const bool locked = ok;
    t0 = ca_os_tick_ms();
    for (u32 r = 0U; r < E139_CONV_REPS && ok; r++) {
        for (u32 row = 0U; row < H && ok; row += lines) {
            const u32 n = (row + lines <= H) ? lines : H - row;
            /* L8: the palette into the CLUT once per frame (as once per update) */
            const ca_dma2d_src_t sr = { .addr = in.base + row * in.stride, .stride = W,
                                        .fmt = l8 ? CA_DMA2D_L8 : CA_DMA2D_RGB565,
                                        .clut = (l8 && row == 0U) ? in.palette : NULL,
                                        .clut_n = 256U };
            const ca_dma2d_dst_t ds = { .addr = buf, .stride = W, .fmt = CA_DMA2D_ARGB8888 };
            ok = ca_dma2d_convert(&sr, &ds, W, n, 100U);
        }
    }
    const u32 hw = ca_os_tick_ms() - t0;

    /* same pixels? the last line again, alone, through the DMA2D */
    u32 bad = 0U;
    if (ok) {
        const ca_dma2d_src_t sr = { .addr = in.base + (H - 1U) * in.stride, .stride = W,
                                    .fmt = l8 ? CA_DMA2D_L8 : CA_DMA2D_RGB565,
                                    .clut = l8 ? in.palette : NULL, .clut_n = 256U };
        const ca_dma2d_dst_t ds = { .addr = buf, .stride = W, .fmt = CA_DMA2D_ARGB8888 };
        ok = ca_dma2d_convert(&sr, &ds, W, 1U, 100U);
        for (u32 i = 0U; ok && i < nref; i++)
            if ((((const u32 *)buf)[i] & 0x00FFFFFFU) != ref[i]) bad++;
    }
    if (locked) ca_dma2d_unlock();
    (void)ca_memory_free(buf, NULL, false);
    CLI_PRINTF_N("E139 conv %ux%u %s->32bpp: sw %u us/frame, dma2d %u-line strips %u us/frame%s",
                 W, H, l8 ? "L8" : "RGB565", sw * 1000U / E139_CONV_REPS, lines, hw * 1000U / E139_CONV_REPS,
                 ok ? "" : " FAILED");
    CLI_PRINTF_N("E139 conv check: %u frames each, pixels differing %u", E139_CONV_REPS, bad);
}
#endif

#if EN_DMA2D && defined(DMA2D) && EN_DMA
/* 'e139 copy' (vnc_add.md D10, double buffer): what copying the drawing
 * buffer to a shown one costs, by engine. A second full buffer does not fit
 * next to this one, so the copies go into a band of E139_CP_ROWS full rows
 * at the same offsets: contiguous MB/s (the whole screen is contiguous),
 * and microseconds per 16x16 tile (a tile is 16 pieces of a row, stride
 * apart). The DMA2D copies a byte view in RGB565 (2 bytes a pixel), so L8
 * and RGB565 go the same way. */
#define E139_CP_ROWS   32U
#define E139_CP_TILES  40U

static bool s_dma_inited;

static u32 cp_rate(u32 bytes, u32 reps, u32 ms)     /* KB/s, 0 if no time */
{
    return (ms == 0U) ? 0U : (u32)(((u64)bytes * reps * 1000U) / ((u64)ms * 1024U));
}

static void e139_copy(void)
{
    ca_fb_info_t in;
    if (!ca_fb_info(s_fb, &in)) { CLI_PUT_N("E139 copy: no framebuffer"); return; }
    const u32 st = in.stride, band = st * E139_CP_ROWS, tb = 16U * (in.bpp / 8U);
    u8 *dst = (u8 *)ca_memory_allocate(band);
    if (dst == NULL) { CLI_PUT_N("E139 copy: no memory for the band"); return; }
    if (!s_dma_inited) { ca_dma_init(); s_dma_inited = true; }
    (void)ca_dma2d_init();
    ca_dma_ch_t ch;
    const bool have_dma = ca_dma_free_channel_m2m(&ch) >= 0;
    const ca_dma2d_src_t sband = { .addr = in.base, .stride = st / 2U, .fmt = CA_DMA2D_RGB565 };
    const ca_dma2d_dst_t dband = { .addr = dst, .stride = st / 2U, .fmt = CA_DMA2D_RGB565 };

    /* contiguous: E139_CP_ROWS full rows */
    u32 t0 = ca_os_tick_ms();
    for (u32 r = 0U; r < 200U; r++) ca_memcpy(dst, in.base, band);
    const u32 c_cpu = cp_rate(band, 200U, ca_os_tick_ms() - t0);
    u32 c_dma = 0U;
    if (have_dma) {
        t0 = ca_os_tick_ms();
        for (u32 r = 0U; r < 200U; r++) {
            (void)ca_dma_copy_inc(ch, in.base, dst, band / 4U, CA_DMA_WIDTH_32);
            (void)ca_dma_wait(ch, 100U);
        }
        c_dma = cp_rate(band, 200U, ca_os_tick_ms() - t0);
    }
    bool ok2d = ca_dma2d_lock(1000U);
    const bool locked = ok2d;
    t0 = ca_os_tick_ms();
    for (u32 r = 0U; r < 200U && ok2d; r++) ok2d = ca_dma2d_copy(&sband, &dband, st / 2U, E139_CP_ROWS, 100U);
    const u32 c_2d = ok2d ? cp_rate(band, 200U, ca_os_tick_ms() - t0) : 0U;

    /* tiles: E139_CP_TILES tiles of the first 16 rows, one by one */
    const u32 nt = (in.width / 16U < E139_CP_TILES) ? in.width / 16U : E139_CP_TILES;
    t0 = ca_os_tick_ms();
    for (u32 r = 0U; r < 100U; r++)
        for (u32 t = 0U; t < nt; t++)
            for (u32 y = 0U; y < 16U; y++)
                ca_memcpy(dst + y * st + t * tb, in.base + y * st + t * tb, tb);
    const u32 t_cpu = (ca_os_tick_ms() - t0) * 1000U / (100U * nt);
    t0 = ca_os_tick_ms();
    for (u32 r = 0U; r < 100U && ok2d; r++)
        for (u32 t = 0U; t < nt && ok2d; t++) {
            const ca_dma2d_src_t s1 = { .addr = in.base + t * tb, .stride = st / 2U, .fmt = CA_DMA2D_RGB565 };
            const ca_dma2d_dst_t d1 = { .addr = dst + t * tb, .stride = st / 2U, .fmt = CA_DMA2D_RGB565 };
            ok2d = ca_dma2d_copy(&s1, &d1, tb / 2U, 16U, 100U);
        }
    const u32 t_2d = ok2d ? (ca_os_tick_ms() - t0) * 1000U / (100U * nt) : 0U;
    if (locked) ca_dma2d_unlock();
    u32 t_dma = 0U;
    if (have_dma) {
        t0 = ca_os_tick_ms();
        for (u32 r = 0U; r < 10U; r++)
            for (u32 t = 0U; t < nt; t++)
                for (u32 y = 0U; y < 16U; y++) {
                    (void)ca_dma_copy_inc(ch, in.base + y * st + t * tb, dst + y * st + t * tb,
                                          tb / 4U, CA_DMA_WIDTH_32);
                    (void)ca_dma_wait(ch, 100U);
                }
        t_dma = (ca_os_tick_ms() - t0) * 1000U / (10U * nt);
        ca_dma_release(ch);
    }
    const u32 bad = (u32)ca_memcmp(dst, in.base, tb);   /* last copy of tile 0, row 0 */
    (void)ca_memory_free(dst, NULL, false);

    const u32 full = in.size;
    CLI_PRINTF_N("E139 copy %ux%u %s, %u B/screen, tile %u B", (u32)in.width, (u32)in.height,
                 in.bpp == 8U ? "L8" : "RGB565", full, 16U * tb);
    CLI_PRINTF_N("E139 copy contiguous KB/s: cpu %u dma %u dma2d %u", c_cpu, c_dma, c_2d);
    CLI_PRINTF_N("E139 copy screen us: cpu %u dma %u dma2d %u",
                 c_cpu ? (u32)((u64)full * 1000000U / ((u64)c_cpu * 1024U)) : 0U,
                 c_dma ? (u32)((u64)full * 1000000U / ((u64)c_dma * 1024U)) : 0U,
                 c_2d ? (u32)((u64)full * 1000000U / ((u64)c_2d * 1024U)) : 0U);
    CLI_PRINTF_N("E139 copy per tile us: cpu %u dma2d %u dma %u (16 rows each)%s",
                 t_cpu, t_2d, t_dma, bad ? " MISMATCH" : "");
}
#endif

/* key/ptr are injected (source local); buf 2h = double buffer at half size;
 * conv: fb -> 32 bpp, software vs DMA2D strips; copy: cost by engine */
#if EN_DMA2D && defined(DMA2D) && EN_DMA
#define E139_HELP "e139      : grid | box <x> <y> <w> <h> <c> | pal <i> <rrggbb> | crc | key <keysym hex> | ptr <x> <y> <buttons> | input on|off | destroy | new | sec auto|tls|none | fmt l8|rgb565 | buf 1|2|2h | conv [lines] | copy"
#elif EN_DMA2D && defined(DMA2D)
#define E139_HELP "e139      : grid | box <x> <y> <w> <h> <c> | pal <i> <rrggbb> | crc | key <keysym hex> | ptr <x> <y> <buttons> | input on|off | destroy | new | sec auto|tls|none | fmt l8|rgb565 | buf 1|2|2h | conv [lines]"
#else
#define E139_HELP "e139      : grid | box <x> <y> <w> <h> <c> | pal <i> <rrggbb> | crc | key <keysym hex> | ptr <x> <y> <buttons> | input on|off | destroy | new | sec auto|tls|none | fmt l8|rgb565 | buf 1|2|2h"
#endif

/* e139 — the line arrives without the command word */
static bool e139_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "grid", 4) == 0) {
        draw_pattern();
        CLI_PUT_N("E139 grid: ok");
    } else if (ca_strncmp(p, "box", 3) == 0) {
        p += 3;
        const i32 x = arg(&p, 10), y = arg(&p, 10), w = arg(&p, 10), h = arg(&p, 10);
        CLI_PRINTF_N("E139 box: %d", ca_fb_fill_rect(s_fb, x, y, w, h, px((u32)arg(&p, 10) & 0xFFU)));
        (void)ca_fb_present(s_fb);
    } else if (ca_strncmp(p, "pal", 3) == 0) {
        p += 3;
        const i32 i = arg(&p, 10);
        const u32 rgb = (u32)arg(&p, 16);
        const bool ok = i >= 0 && i < 256 && ca_fb_set_palette(s_fb, (u16)i, 1U, &rgb);
        CLI_PRINTF_N("E139 pal %d = %06X: %s", i, rgb & 0xFFFFFFU, ok ? "ok" : "refused");
    } else if (ca_strncmp(p, "crc", 3) == 0) {
        ca_fb_info_t in;
        u32 h = 2166136261U;
        if (ca_fb_info(s_fb, &in))
            for (u32 i = 0U; i < in.size; i++) { h ^= in.base[i]; h *= 16777619U; }
        CLI_PRINTF_N("E139 crc %08X", h);
    } else if (ca_strncmp(p, "key", 3) == 0) {
        p += 3;
        const u32 sym = (u32)arg(&p, 16);
        const bool ok = ca_vnc_inject_key(CA_VNC_SRC_LOCAL, true, sym) &&
                        ca_vnc_inject_key(CA_VNC_SRC_LOCAL, false, sym);
        CLI_PRINTF_N("E139 inject key: %s", ok ? "ok" : "no callback");
    } else if (ca_strncmp(p, "ptr", 3) == 0) {
        p += 3;
        const i32 x = arg(&p, 10), y = arg(&p, 10);
        const u8 b = (u8)arg(&p, 10);
        CLI_PRINTF_N("E139 inject ptr: %s",
                     ca_vnc_inject_pointer(CA_VNC_SRC_LOCAL, b, (u16)x, (u16)y) ? "ok" : "no callback");
    } else if (ca_strncmp(p, "input", 5) == 0) {
        const bool on = ca_strncmp(ca_skip_sp(p + 5), "on", 2) == 0;
        ca_vnc_service_set_client_input(on);
        CLI_PRINTF_N("E139 client input: %s", on ? "on" : "off (view only)");
    } else if (ca_strncmp(p, "sec", 3) == 0) {
        p = ca_skip_sp(p + 3);
        s_sec = (ca_strncmp(p, "none", 4) == 0) ? CA_VNC_SECURITY_NONE :
                (ca_strncmp(p, "tls", 3) == 0)  ? CA_VNC_SECURITY_TLS : CA_VNC_SECURITY_AUTO;
        CLI_PRINTF_N("E139 sec: %s (next 'e139 new')", s_sec == CA_VNC_SECURITY_NONE ? "none" :
                     (s_sec == CA_VNC_SECURITY_TLS ? "tls" : "auto"));
#if EN_DMA2D && defined(DMA2D)
#if EN_DMA2D && defined(DMA2D) && EN_DMA
    } else if (ca_strncmp(p, "copy", 4) == 0) {
        e139_copy();
#endif
    } else if (ca_strncmp(p, "conv", 4) == 0) {
        p += 4;
        e139_conv((u32)arg(&p, 10));
#endif
    } else if (ca_strncmp(p, "fmt", 3) == 0) {
        p = ca_skip_sp(p + 3);
        s_fmt = (ca_strncmp(p, "rgb565", 6) == 0) ? CA_FB_FMT_RGB565 : CA_FB_FMT_L8;
        s_w = FB_WIDTH;
        s_h = (s_fmt == CA_FB_FMT_RGB565) ? FB_HEIGHT / 2 : FB_HEIGHT;
        CLI_PRINTF_N("E139 fmt: %s %dx%d (next 'e139 new')",
                     s_fmt == CA_FB_FMT_RGB565 ? "rgb565" : "l8", s_w, s_h);
    } else if (ca_strncmp(p, "destroy", 7) == 0) {
        const u32 before = ca_memory_get_avail_memory();
        const bool ok = ca_vnc_service_destroy();
        const u32 after = ca_memory_get_avail_memory();
        CLI_PRINTF_N("E139 destroy: %s, heap free %u -> %u (+%u), fb %s", ok ? "ok" : "FAILED",
                     before, after, after - before,
                     ca_fb_fill_rect(s_fb, 0, 0, 1, 1, 0U) < 0 ? "stale" : "STILL VALID");
    } else if (ca_strncmp(p, "buf", 3) == 0) {
        /* 1 | 2 (full size: falls back to 1 where 2 do not fit) | 2h (half size) */
        p = ca_skip_sp(p + 3);
        const bool half = p[0] == '2' && p[1] == 'h';
        s_buf = (p[0] == '2') ? 2U : 1U;
        s_w = half ? FB_WIDTH / 2 : FB_WIDTH;
        s_h = (s_fmt == CA_FB_FMT_RGB565) ? FB_HEIGHT / 2 : FB_HEIGHT;
        if (half) s_h /= 2;
        CLI_PRINTF_N("E139 buf: %u buffer(s) asked, %dx%d (next 'e139 new')", (u32)s_buf, s_w, s_h);
    } else if (ca_strncmp(p, "new", 3) == 0) {
        const u32 before = ca_memory_get_avail_memory();
        s_fb = ca_fb_create((u16)s_w, (u16)s_h, s_fmt, s_buf);
        if (s_fb != CA_FB_NONE) draw_pattern();
        const bool ok = s_fb != CA_FB_NONE && vnc_up();
        ca_os_sleep(1000U);                 /* the task takes its buffers (TLS: after checking the key) */
        const u32 after = ca_memory_get_avail_memory();
        ca_fb_info_t fi;
        const u32 nb = ca_fb_info(s_fb, &fi) ? fi.buffers : 0U;
        CLI_PRINTF_N("E139 new: %s, heap free %u -> %u (-%u), %u buffer(s)", ok ? "ok" : "FAILED",
                     before, after, before - after, nb);
    } else {
        return false;                   /* cli prints the help line */
    }
    return true;
}
static const ca_cli_cmd_t s_e139_cmds[] = {
    { "e139", E139_HELP, { e139_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_e139_table = {
    "e139", s_e139_cmds, (u16)(sizeof(s_e139_cmds) / sizeof(s_e139_cmds[0])), NULL
};
#endif

/* ============================================================================
 * GPIO ETH (as ex137)
 * ============================================================================ */
static void eth_gpio_init(void)
{
#if MCU_ID == 773
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_B,  6U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_G,  6U },
        { CA_GPIO_PORT_A,  7U }, { CA_GPIO_PORT_G,  4U }, { CA_GPIO_PORT_G,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U }, { CA_GPIO_PORT_G, 12U },
    };
#else
    static const struct { u8 port; u8 pin; } pins[] = {
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
PRIVATE_STRING(nofb, "ex139: no memory for the framebuffer");

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
        ca_cli_register_table(&s_e139_table);
    )
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    s_fb = ca_fb_create(FB_WIDTH, FB_HEIGHT, CA_FB_FMT_L8, 1U);
    if (s_fb == CA_FB_NONE) PUT_N(&priv_nofb);
    else draw_pattern();

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
    ca_os_run(true);
}

#else

PRIVATE_STRING(m139a, "=== Example 139: caOS VNC server on a framebuffer ===");
PRIVATE_STRING(m139b, "DISABLED - requires EN_ETH / EN_NET / EN_ARP_SERVICE / EN_DHCP_SERVICE / EN_NET_DHCP / EN_TCP_SERVICE / EN_FB / EN_VNC_SERVICE");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m139a);
    PUT_N(&priv_m139b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
