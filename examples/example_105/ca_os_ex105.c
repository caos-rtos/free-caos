// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex105.c
 * @brief          : Example 105 — DMA2D against a software model (F767, H753,
 *                   U575)
 ******************************************************************************
 * @details
 * Every DMA2D operation is done again in C, by a model written from the
 * reference manual (channel widening by bit replication, the blender with
 * divisions rounded down, the output packing), and the two results are
 * compared byte for byte over the WHOLE destination: the rectangle must
 * equal the model, every pixel around it must still hold the sentinel. No
 * wire, no display: RAM to RAM.
 *
 *   d2d test <group>   fill | copy | pfc | alpha | blend | edge | ctrl
 *   d2d all            every group, one line each:
 *                      "D2D ALL PASS (n/n)" / "D2D ALL FAIL (n/n)"
 *   d2d bench          DMA2D against the CPU, in cycles (no verdict)
 *
 * Images are 80x60; the sources are read from a sub-rectangle at (4,5),
 * written at (6,7) of the destination, 38x23 unless a case says otherwise.
 * Source bytes and CLUTs are pseudo-random, so every format reads noise.
 ******************************************************************************
 */

#include "caos.h"

#if EN_DMA2D

#define IW          80U             /* image width (all buffers) */
#define IH          60U
#define SX          4U              /* source rectangle origin (even: 4-bit formats) */
#define SY          5U
#define DX          6U              /* destination rectangle origin */
#define DY          7U
#define RW          38U             /* default rectangle */
#define RH          23U
#define SENTINEL    0xA5U
#define TMO_MS      100U
#define SHOW_MAX    3U              /* failures detailed per group */

#if defined(DMA2D)
#define BUF_WORDS   (IW * IH)
#else
/* No DMA2D (F446 has 128 KB): the example stops at boot, the buffers only
 * have to link */
#define BUF_WORDS   16U
#endif

/* Static, word aligned: on H7 the stacks are in DTCM, out of DMA2D reach */
static u32 s_fg[BUF_WORDS];
static u32 s_bg[BUF_WORDS];
static u32 s_dst[BUF_WORDS];
static u32 s_ref[BUF_WORDS];
static u32 s_clut_fg[256];
static u32 s_clut_bg[256];

static const char *const s_fmt_name[] = {
    "ARGB8888", "RGB888", "RGB565", "ARGB1555", "ARGB4444",
    "L8", "AL44", "AL88", "L4", "A8", "A4"
};

static u32 s_shown;                 /* failures printed in this group */

/* ---------------------------------------------------------------- pixels */

static u32 px_get(const void *buf, u32 stride, ca_dma2d_fmt_t f, u32 x, u32 y)
{
    const u32 bpp = ca_dma2d_bpp(f);
    const u32 bit = (y * stride + x) * bpp;
    const u8 *p = (const u8 *)buf + bit / 8U;
    switch (bpp) {
    case 32U: return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    case 24U: return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
    case 16U: return (u32)p[0] | ((u32)p[1] << 8);
    case 8U:  return p[0];
    default:  return (bit & 4U) ? (u32)(p[0] >> 4) : (u32)(p[0] & 0x0FU);  /* pixel 0 low nibble */
    }
}

static void px_put(void *buf, u32 stride, ca_dma2d_fmt_t f, u32 x, u32 y, u32 v)
{
    const u32 bpp = ca_dma2d_bpp(f);
    const u32 bit = (y * stride + x) * bpp;
    u8 *p = (u8 *)buf + bit / 8U;
    switch (bpp) {
    case 32U: p[3] = (u8)(v >> 24); /* fall through */
    case 24U: p[2] = (u8)(v >> 16); /* fall through */
    case 16U: p[1] = (u8)(v >> 8);  /* fall through */
    case 8U:  p[0] = (u8)v; break;
    default:
        p[0] = (bit & 4U) ? (u8)((p[0] & 0x0FU) | (v << 4)) : (u8)((p[0] & 0xF0U) | (v & 0x0FU));
        break;
    }
}

static void *px_addr(void *buf, ca_dma2d_fmt_t f, u32 x, u32 y)
{
    return (u8 *)buf + ((y * IW + x) * ca_dma2d_bpp(f)) / 8U;
}

/* ----------------------------------------------------------------- model */

static u32 w5(u32 v) { return (v << 3) | (v >> 2); }
static u32 w6(u32 v) { return (v << 2) | (v >> 4); }
static u32 w4(u32 v) { return (v << 4) | v; }
static u32 argb(u32 a, u32 r, u32 g, u32 b) { return (a << 24) | (r << 16) | (g << 8) | b; }

/* Input PFC: raw pixel -> ARGB8888, then RBS, AI and the alpha mode */
static u32 model_in(const ca_dma2d_src_t *s, const u32 *clut, u32 raw)
{
    u32 c;
    switch (s->fmt) {
    case CA_DMA2D_ARGB8888: c = raw; break;
    case CA_DMA2D_RGB888:   c = 0xFF000000U | raw; break;
    case CA_DMA2D_RGB565:   c = argb(0xFFU, w5(raw >> 11), w6((raw >> 5) & 0x3FU), w5(raw & 0x1FU)); break;
    case CA_DMA2D_ARGB1555: c = argb((raw & 0x8000U) ? 0xFFU : 0U, w5((raw >> 10) & 0x1FU),
                                     w5((raw >> 5) & 0x1FU), w5(raw & 0x1FU)); break;
    case CA_DMA2D_ARGB4444: c = argb(w4(raw >> 12), w4((raw >> 8) & 0xFU), w4((raw >> 4) & 0xFU), w4(raw & 0xFU)); break;
    case CA_DMA2D_L8:
    case CA_DMA2D_L4:       c = clut[raw]; break;
    case CA_DMA2D_AL44:     c = (w4(raw >> 4) << 24) | (clut[raw & 0xFU] & 0xFFFFFFU); break;
    case CA_DMA2D_AL88:     c = ((raw >> 8) << 24) | (clut[raw & 0xFFU] & 0xFFFFFFU); break;
    case CA_DMA2D_A8:       c = (raw << 24) | (s->color & 0xFFFFFFU); break;
    default:                c = (w4(raw) << 24) | (s->color & 0xFFFFFFU); break;   /* A4 */
    }
    if (s->flags & CA_DMA2D_RBS)
        c = (c & 0xFF00FF00U) | ((c >> 16) & 0xFFU) | ((c & 0xFFU) << 16);
    u32 a = c >> 24, k = s->alpha;
    if (s->flags & CA_DMA2D_AI) { a = 255U - a; k = 255U - k; }
    if (s->am == CA_DMA2D_ALPHA_REPLACE) a = k;
    else if (s->am == CA_DMA2D_ALPHA_MULT) a = (a * k) / 255U;
    return (a << 24) | (c & 0xFFFFFFU);
}

/* Output PFC: ARGB8888 -> raw, the low bits dropped */
static u32 model_out(ca_dma2d_fmt_t f, u8 flags, u32 c)
{
    if (flags & CA_DMA2D_RBS)
        c = (c & 0xFF00FF00U) | ((c >> 16) & 0xFFU) | ((c & 0xFFU) << 16);
    u32 a = c >> 24;
    if (flags & CA_DMA2D_AI) a = 255U - a;
    const u32 r = (c >> 16) & 0xFFU, g = (c >> 8) & 0xFFU, b = c & 0xFFU;
    switch (f) {
    case CA_DMA2D_ARGB8888: return (a << 24) | (c & 0xFFFFFFU);
    case CA_DMA2D_RGB888:   return c & 0xFFFFFFU;
    case CA_DMA2D_RGB565:   return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    case CA_DMA2D_ARGB1555: return ((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    default:                return ((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
    }
}

/* RM0410 9.3.6, every division rounded down */
static u32 model_blend(u32 f, u32 b)
{
    const u32 af = f >> 24, ab = b >> 24;
    const u32 am = (af * ab) / 255U;
    const u32 ao = af + ab - am;
    u32 out = ao << 24;
    for (u32 sh = 0U; sh <= 16U; sh += 8U) {
        const u32 cf = (f >> sh) & 0xFFU, cb = (b >> sh) & 0xFFU;
        const u32 c = (ao == 0U) ? 0U : (cf * af + cb * ab - cb * am) / ao;
        out |= (c & 0xFFU) << sh;
    }
    return out;
}

/* ------------------------------------------------------------- the frame */

static void noise(void *buf, u32 bytes, u32 seed)
{
    u8 *p = (u8 *)buf;
    u32 r = seed;
    for (u32 i = 0U; i < bytes; i++) {
        r = r * 1664525U + 1013904223U;
        p[i] = (u8)(r >> 24);
    }
}

static void prepare(u32 seed)
{
    noise(s_fg, sizeof(s_fg), seed);
    noise(s_bg, sizeof(s_bg), seed ^ 0x5A5A5A5AU);
    noise(s_clut_fg, sizeof(s_clut_fg), seed + 7U);
    noise(s_clut_bg, sizeof(s_clut_bg), seed + 11U);
    u8 *d = (u8 *)s_dst, *r = (u8 *)s_ref;
    for (u32 i = 0U; i < sizeof(s_dst); i++) { d[i] = SENTINEL; r[i] = SENTINEL; }
}

static ca_dma2d_src_t src_at(u32 *buf, const u32 *clut, ca_dma2d_fmt_t f, u32 x, u32 y)
{
    ca_dma2d_src_t s = { 0 };
    s.addr   = px_addr(buf, f, x, y);
    s.stride = IW;
    s.fmt    = f;
    s.color  = 0x3C7AE1U;
    s.clut   = clut;
    s.clut_n = 256U;
    return s;
}

static ca_dma2d_dst_t dst_at(ca_dma2d_fmt_t f, u32 x, u32 y)
{
    ca_dma2d_dst_t d = { px_addr(s_dst, f, x, y), IW, f, 0U };
    return d;
}

/* The whole destination against the model; the first difference shown */
static bool verdict(const char *label, bool ran, ca_dma2d_fmt_t f)
{
    const u8 *d = (const u8 *)s_dst, *r = (const u8 *)s_ref;
    u32 i = 0U;
    while (i < sizeof(s_dst) && d[i] == r[i]) i++;
    const bool ok = ran && i == sizeof(s_dst);
    if (!ok && s_shown < SHOW_MAX) {
        s_shown++;
        if (!ran) {
            CLI_PRINTF_N("D2D   %s: refused or failed, errors %02X", label, ca_dma2d_errors());
        } else {
            const u32 pix = (i * 8U) / ca_dma2d_bpp(f);
            const u32 x = pix % IW, y = pix / IW;
            CLI_PRINTF_N("D2D   %s: (%u,%u) got %08X want %08X%s", label, x, y,
                         px_get(s_dst, IW, f, x, y), px_get(s_ref, IW, f, x, y),
                         (x < DX || y < DY) ? "  (outside the rectangle)" : "");
        }
    }
    return ok;
}

static bool model_convert(const ca_dma2d_src_t *s, const u32 *buf, const u32 *clut,
                          u32 sx, u32 sy, const ca_dma2d_dst_t *d, u32 dx, u32 dy, u32 w, u32 h)
{
    for (u32 y = 0U; y < h; y++)
        for (u32 x = 0U; x < w; x++) {
            const u32 c = model_in(s, clut, px_get(buf, IW, s->fmt, sx + x, sy + y));
            px_put(s_ref, IW, d->fmt, dx + x, dy + y, model_out(d->fmt, d->flags, c));
        }
    return true;
}

/* One conversion case, hardware and model */
static bool pfc_case(const char *grp, ca_dma2d_fmt_t in, ca_dma2d_fmt_t out, u8 sflags, u8 dflags,
                     ca_dma2d_am_t am, u8 alpha, u32 w, u32 h, u32 seed)
{
    prepare(seed);
    ca_dma2d_src_t s = src_at(s_fg, s_clut_fg, in, SX, SY);
    s.flags = sflags; s.am = am; s.alpha = alpha;
    ca_dma2d_dst_t d = dst_at(out, DX, DY);
    d.flags = dflags;
    const bool ran = ca_dma2d_convert(&s, &d, w, h, TMO_MS);
    model_convert(&s, s_fg, s_clut_fg, SX, SY, &d, DX, DY, w, h);
    static char label[64];
    ca_snprintf(label, sizeof(label), "%s %s->%s%s%s", grp, s_fmt_name[in], s_fmt_name[out],
                (sflags & CA_DMA2D_CLUT_CPU) ? " cpu-clut" : "",
                (am == CA_DMA2D_ALPHA_REPLACE) ? " am=rep" : (am == CA_DMA2D_ALPHA_MULT) ? " am=mul" : "");
    return verdict(label, ran, out);
}

/* ---------------------------------------------------------------- groups */

typedef struct { u32 good, runs; } tally_t;

static void count(tally_t *t, bool ok) { t->runs++; if (ok) t->good++; }

static tally_t g_fill(void)
{
    tally_t t = { 0 };
    for (u32 f = 0U; f < CA_DMA2D_OUT_FMTS; f++) {
        prepare(100U + f);
        const u32 bpp = ca_dma2d_bpp((ca_dma2d_fmt_t)f);
        const u32 color = (bpp == 32U) ? 0x8E3C7A51U : (bpp == 24U) ? 0x3C7A51U : 0x7A51U;
        ca_dma2d_dst_t d = dst_at((ca_dma2d_fmt_t)f, DX, DY);
        const bool ran = ca_dma2d_fill(&d, RW, RH, color, TMO_MS);
        for (u32 y = 0U; y < RH; y++)
            for (u32 x = 0U; x < RW; x++) px_put(s_ref, IW, (ca_dma2d_fmt_t)f, DX + x, DY + y, color);
        static char label[32];
        ca_snprintf(label, sizeof(label), "fill %s", s_fmt_name[f]);
        count(&t, verdict(label, ran, (ca_dma2d_fmt_t)f));
    }
    return t;
}

static tally_t g_copy(void)
{
    static const ca_dma2d_fmt_t fm[] = { CA_DMA2D_ARGB8888, CA_DMA2D_RGB888, CA_DMA2D_RGB565,
                                         CA_DMA2D_L8, CA_DMA2D_L4 };
    tally_t t = { 0 };
    for (u32 k = 0U; k < 5U; k++) {
        const ca_dma2d_fmt_t f = fm[k];
        prepare(200U + k);
        ca_dma2d_src_t s = src_at(s_fg, NULL, f, SX, SY);
        ca_dma2d_dst_t d = dst_at(f, DX, DY);
        const bool ran = ca_dma2d_copy(&s, &d, RW, RH, TMO_MS);
        for (u32 y = 0U; y < RH; y++)
            for (u32 x = 0U; x < RW; x++)
                px_put(s_ref, IW, f, DX + x, DY + y, px_get(s_fg, IW, f, SX + x, SY + y));
        static char label[32];
        ca_snprintf(label, sizeof(label), "copy %s", s_fmt_name[f]);
        count(&t, verdict(label, ran, f));
    }
    return t;
}

static tally_t g_pfc(void)
{
    tally_t t = { 0 };
    for (u32 in = 0U; in <= (u32)CA_DMA2D_A4; in++)
        for (u32 out = 0U; out < CA_DMA2D_OUT_FMTS; out++)
            count(&t, pfc_case("pfc", (ca_dma2d_fmt_t)in, (ca_dma2d_fmt_t)out, 0U, 0U,
                               CA_DMA2D_ALPHA_KEEP, 0U, RW, RH, 300U + in * 8U + out));
    /* The CLUT written by the CPU instead of loaded by the DMA2D */
    static const ca_dma2d_fmt_t ix[] = { CA_DMA2D_L8, CA_DMA2D_AL44, CA_DMA2D_AL88, CA_DMA2D_L4 };
    for (u32 k = 0U; k < 4U; k++)
        count(&t, pfc_case("pfc", ix[k], CA_DMA2D_ARGB8888, CA_DMA2D_CLUT_CPU, 0U,
                           CA_DMA2D_ALPHA_KEEP, 0U, RW, RH, 400U + k));
    return t;
}

static tally_t g_alpha(void)
{
    tally_t t = { 0 };
    const ca_dma2d_fmt_t in = CA_DMA2D_ARGB4444, out = CA_DMA2D_ARGB8888;
    count(&t, pfc_case("alpha", in, out, 0U, 0U, CA_DMA2D_ALPHA_KEEP, 0x80U, RW, RH, 500U));
    count(&t, pfc_case("alpha", in, out, 0U, 0U, CA_DMA2D_ALPHA_REPLACE, 0x80U, RW, RH, 501U));
    count(&t, pfc_case("alpha", in, out, 0U, 0U, CA_DMA2D_ALPHA_MULT, 0x80U, RW, RH, 502U));
    count(&t, pfc_case("alpha ai-in", in, out, CA_DMA2D_AI, 0U, CA_DMA2D_ALPHA_KEEP, 0U, RW, RH, 503U));
    count(&t, pfc_case("alpha rbs-in", in, out, CA_DMA2D_RBS, 0U, CA_DMA2D_ALPHA_KEEP, 0U, RW, RH, 504U));
    count(&t, pfc_case("alpha ai-out", in, out, 0U, CA_DMA2D_AI, CA_DMA2D_ALPHA_KEEP, 0U, RW, RH, 505U));
    count(&t, pfc_case("alpha rbs-out", in, out, 0U, CA_DMA2D_RBS, CA_DMA2D_ALPHA_KEEP, 0U, RW, RH, 506U));
    return t;
}

static tally_t g_blend(void)
{
    static const ca_dma2d_fmt_t fgf[] = { CA_DMA2D_ARGB8888, CA_DMA2D_ARGB4444, CA_DMA2D_A8 };
    static const ca_dma2d_fmt_t bgf[] = { CA_DMA2D_RGB565, CA_DMA2D_ARGB8888 };
    tally_t t = { 0 };
    u32 seed = 600U;
    for (u32 i = 0U; i < 3U; i++)
        for (u32 j = 0U; j < 2U; j++)
            for (u32 am = 0U; am <= (u32)CA_DMA2D_ALPHA_MULT; am++) {
                prepare(seed++);
                ca_dma2d_src_t fg = src_at(s_fg, s_clut_fg, fgf[i], SX, SY);
                fg.am = (ca_dma2d_am_t)am; fg.alpha = 0x9CU;
                ca_dma2d_src_t bg = src_at(s_bg, s_clut_bg, bgf[j], SX + 2U, SY + 1U);
                ca_dma2d_dst_t d = dst_at(bgf[j], DX, DY);
                const bool ran = ca_dma2d_blend(&fg, &bg, &d, RW, RH, TMO_MS);
                for (u32 y = 0U; y < RH; y++)
                    for (u32 x = 0U; x < RW; x++) {
                        const u32 f = model_in(&fg, s_clut_fg, px_get(s_fg, IW, fg.fmt, SX + x, SY + y));
                        const u32 b = model_in(&bg, s_clut_bg, px_get(s_bg, IW, bg.fmt, SX + 2U + x, SY + 1U + y));
                        px_put(s_ref, IW, d.fmt, DX + x, DY + y, model_out(d.fmt, 0U, model_blend(f, b)));
                    }
                static char label[48];
                ca_snprintf(label, sizeof(label), "blend %s over %s am=%u",
                            s_fmt_name[fgf[i]], s_fmt_name[bgf[j]], am);
                count(&t, verdict(label, ran, d.fmt));
            }
    return t;
}

static tally_t g_edge(void)
{
    tally_t t = { 0 };
    count(&t, pfc_case("edge w=1", CA_DMA2D_ARGB8888, CA_DMA2D_RGB565, 0U, 0U, CA_DMA2D_ALPHA_KEEP, 0U, 1U, RH, 700U));
    count(&t, pfc_case("edge h=1", CA_DMA2D_ARGB8888, CA_DMA2D_RGB565, 0U, 0U, CA_DMA2D_ALPHA_KEEP, 0U, RW, 1U, 701U));
    count(&t, pfc_case("edge w=37", CA_DMA2D_ARGB4444, CA_DMA2D_ARGB1555, 0U, 0U, CA_DMA2D_ALPHA_KEEP, 0U, 37U, RH, 702U));

    /* The whole frame: zero line offsets on both sides */
    prepare(703U);
    ca_dma2d_src_t s = src_at(s_fg, NULL, CA_DMA2D_RGB565, 0U, 0U);
    ca_dma2d_dst_t d = dst_at(CA_DMA2D_ARGB8888, 0U, 0U);
    const bool ran = ca_dma2d_convert(&s, &d, IW, IH, TMO_MS);
    model_convert(&s, s_fg, NULL, 0U, 0U, &d, 0U, 0U, IW, IH);
    count(&t, verdict("edge full frame RGB565->ARGB8888", ran, d.fmt));

    /* What the driver must refuse, and say why */
    prepare(704U);
    s = src_at(s_fg, s_clut_fg, CA_DMA2D_L4, SX, SY);
    d = dst_at(CA_DMA2D_ARGB8888, DX, DY);
    bool ok = !ca_dma2d_convert(&s, &d, 37U, RH, TMO_MS) && (ca_dma2d_errors() & CA_DMA2D_ERR_ARG);
    count(&t, verdict("edge L4 odd width refused", ok, d.fmt));

    s = src_at(s_fg, NULL, CA_DMA2D_ARGB8888, SX, SY);
    s.addr = (const u8 *)s.addr + 2;
    ok = !ca_dma2d_convert(&s, &d, RW, RH, TMO_MS) && (ca_dma2d_errors() & CA_DMA2D_ERR_ARG);
    count(&t, verdict("edge misaligned ARGB8888 refused", ok, d.fmt));
    return t;
}

static tally_t g_ctrl(void)
{
    tally_t t = { 0 };

    /* Completion by interrupt: started, then waited on the semaphore */
    prepare(800U);
    ca_dma2d_src_t s = src_at(s_fg, NULL, CA_DMA2D_ARGB8888, SX, SY);
    ca_dma2d_dst_t d = dst_at(CA_DMA2D_RGB565, DX, DY);
    bool ran = ca_dma2d_convert(&s, &d, RW, RH, CA_NO_WAIT_TIME) && ca_dma2d_wait(TMO_MS);
    model_convert(&s, s_fg, NULL, SX, SY, &d, DX, DY, RW, RH);
    count(&t, verdict("ctrl irq completion", ran, d.fmt));

    /* Abort a whole-frame fill right after its start: it must stop part way,
     * not raise a completion, and the next operation must run normally */
    prepare(801U);
    d = dst_at(CA_DMA2D_ARGB8888, 0U, 0U);
    const bool started = ca_dma2d_fill(&d, IW, IH, 0x11223344U, CA_NO_WAIT_TIME);
    ca_dma2d_abort();
    u32 written = 0U;
    for (u32 i = 0U; i < BUF_WORDS; i++) if (s_dst[i] != 0xA5A5A5A5U) written++;
    const bool partial = started && !ca_dma2d_busy() && written > 0U && written < IW * IH;
    if (!partial && s_shown < SHOW_MAX) {
        s_shown++;
        CLI_PRINTF_N("D2D   ctrl abort: %u of %u pixels written (want some, not all)", written, IW * IH);
    }
    count(&t, partial);

    prepare(802U);
    d = dst_at(CA_DMA2D_RGB565, DX, DY);
    ran = ca_dma2d_fill(&d, RW, RH, 0x1234U, TMO_MS);
    for (u32 y = 0U; y < RH; y++)
        for (u32 x = 0U; x < RW; x++) px_put(s_ref, IW, d.fmt, DX + x, DY + y, 0x1234U);
    count(&t, verdict("ctrl fill after abort", ran, d.fmt));

    /* Bus error: a source no bus master but the CPU reaches - the Cortex-M
     * private peripheral bus. Not the FMC bank: with the FMC off the U575
     * read 0x60000000 as zeros, no error (unmapped gaps 0xA0000000,
     * 0x5003F000, 0x1FFF0000 all gave TE; `d2d probe <addr>`) */
    prepare(803U);
    s = src_at(s_fg, NULL, CA_DMA2D_ARGB8888, SX, SY);
    s.addr = (const void *)0xE0000000U;
    d = dst_at(CA_DMA2D_ARGB8888, DX, DY);
    const bool failed = !ca_dma2d_convert(&s, &d, RW, RH, TMO_MS);
    const u32 err = ca_dma2d_errors();
    const bool te = failed && (err & CA_DMA2D_ERR_TE) != 0U;
    if (!te && s_shown < SHOW_MAX) {
        s_shown++;
        CLI_PRINTF_N("D2D   ctrl bus error: %s, errors %02X (want TE 01)", failed ? "failed" : "ran", err);
    }
    count(&t, te);
    return t;
}

typedef struct { const char *name; tally_t (*run)(void); } group_t;

static const group_t s_groups[] = {
    { "fill", g_fill }, { "copy", g_copy }, { "pfc", g_pfc }, { "alpha", g_alpha },
    { "blend", g_blend }, { "edge", g_edge }, { "ctrl", g_ctrl },
};
#define N_GROUPS (sizeof(s_groups) / sizeof(s_groups[0]))

static bool run_group(const group_t *g)
{
    s_shown = 0U;
    const tally_t t = g->run();
    const bool ok = t.good == t.runs;
    CLI_PRINTF_N("D2D %-6s %3u/%3u  %s", g->name, t.good, t.runs, ok ? "PASS" : "FAIL");
    return ok;
}

static void ex105_all(void)
{
    u32 good = 0U;
    for (u32 i = 0U; i < N_GROUPS; i++) if (run_group(&s_groups[i])) good++;
    CLI_PRINTF_N("D2D ALL %s  (%u/%u)", (good == N_GROUPS) ? "PASS" : "FAIL", good, (u32)N_GROUPS);
}

/* ----------------------------------------------------------------- bench */

/* Cycles of one DMA2D operation started with CA_NO_WAIT_TIME, spun on
 * START with interrupts off (the M7 CYCCNT stops in WFI, and a console
 * write inside the window would be measured too) */
static u32 bench_hw(bool blend, const ca_dma2d_src_t *fg, const ca_dma2d_src_t *bg,
                    const ca_dma2d_dst_t *d)
{
    __disable_irq();
    const u32 t0 = DWT->CYCCNT;
    const bool ok = blend ? ca_dma2d_blend(fg, bg, d, IW, IH, CA_NO_WAIT_TIME)
                          : ca_dma2d_copy(fg, d, IW, IH, CA_NO_WAIT_TIME);
    while (ok && ca_dma2d_busy()) { }
    const u32 cyc = DWT->CYCCNT - t0;
    __enable_irq();
    (void)ca_dma2d_wait(TMO_MS);            /* the completion the interrupt posted */
    return ok ? cyc : 0U;
}

static void ex105_bench(void)
{
    prepare(900U);
    ca_dma2d_src_t fg = src_at(s_fg, NULL, CA_DMA2D_ARGB8888, 0U, 0U);
    ca_dma2d_src_t bg = src_at(s_bg, NULL, CA_DMA2D_RGB565, 0U, 0U);
    ca_dma2d_dst_t d  = dst_at(CA_DMA2D_ARGB8888, 0U, 0U);
    ca_os_sleep(30U);                       /* the console sent the echo */

    const u32 hw_copy = bench_hw(false, &fg, NULL, &d);
    __disable_irq();
    u32 t0 = DWT->CYCCNT;
    for (u32 i = 0U; i < BUF_WORDS; i++) s_ref[i] = s_fg[i];
    const u32 cpu_copy = DWT->CYCCNT - t0;
    __enable_irq();

    d = dst_at(CA_DMA2D_RGB565, 0U, 0U);
    const u32 hw_blend = bench_hw(true, &fg, &bg, &d);
    __disable_irq();
    t0 = DWT->CYCCNT;
    for (u32 y = 0U; y < IH; y++)
        for (u32 x = 0U; x < IW; x++) {
            const u32 f = model_in(&fg, NULL, px_get(s_fg, IW, fg.fmt, x, y));
            const u32 b = model_in(&bg, NULL, px_get(s_bg, IW, bg.fmt, x, y));
            px_put(s_ref, IW, d.fmt, x, y, model_out(d.fmt, 0U, model_blend(f, b)));
        }
    const u32 cpu_blend = DWT->CYCCNT - t0;
    __enable_irq();

    CLI_PRINTF_N("D2D bench %ux%u, %u MHz core", IW, IH, SystemCoreClock / 1000000U);
    CLI_PRINTF_N("D2D   copy  ARGB8888           dma2d %7u cyc  cpu word loop %7u cyc", hw_copy, cpu_copy);
    CLI_PRINTF_N("D2D   blend ARGB8888 on RGB565  dma2d %7u cyc  cpu model     %7u cyc", hw_blend, cpu_blend);
}

/* ------------------------------------------------------------------- CLI */

#if ENABLE_CLI
static void ex105_usage(void)
{
    CLI_PUT_N("d2d       : test <fill|copy|pfc|alpha|blend|edge|ctrl> | all | bench | probe <hex addr>");
}

static bool ex105_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { ex105_usage(); return false; }
    if (ca_strncmp(p, "d2d", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "all", 3) == 0)   { ex105_all(); return true; }
    if (ca_strncmp(p, "bench", 5) == 0) { ex105_bench(); return true; }
    if (ca_strncmp(p, "probe", 5) == 0) {
        /* Diagnosis: what the DMA2D gets reading 4x1 ARGB8888 pixels there */
        const u32 a = (u32)ca_strtol(ca_skip_sp(p + 5), 16);
        prepare(1U);
        ca_dma2d_src_t s = src_at(s_fg, NULL, CA_DMA2D_ARGB8888, 0U, 0U);
        s.addr = (const void *)a;
        ca_dma2d_dst_t d = dst_at(CA_DMA2D_ARGB8888, 0U, 0U);
        const bool ok = ca_dma2d_convert(&s, &d, 4U, 1U, TMO_MS);
        CLI_PRINTF_N("D2D probe %08X: %s, errors %02X, read %08X %08X", a, ok ? "ok" : "failed",
                     ca_dma2d_errors(), s_dst[0], s_dst[1]);
        return true;
    }
    if (ca_strncmp(p, "test", 4) == 0) {
        p = ca_skip_sp(p + 4);
        for (u32 i = 0U; i < N_GROUPS; i++) {
            const u32 n = (u32)ca_strlen(s_groups[i].name);
            if (ca_strncmp(p, s_groups[i].name, n) == 0) { (void)run_group(&s_groups[i]); return true; }
        }
    }
    ex105_usage();
    return true;
}
static ca_cli_node_t s_cli = { ex105_cli, NULL };
#endif

PRIVATE_STRING(why, "ex105: this part has no DMA2D (F767, H753 and U575 have it)");

void ca_os_configure(void)
{
    if (!ca_dma2d_init()) {
        /* The line the bench matches to know this example cannot run on
           this part. The reason follows it. */
        CA_EXAMPLE_UNSUPPORTED();
        PUT_N(&priv_why);
        return;
    }

    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_cli);
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else  /* EN_DMA2D */

PRIVATE_STRING(why, "ex105: built without EN_DMA2D");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_DMA2D */
