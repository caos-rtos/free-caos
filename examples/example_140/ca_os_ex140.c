// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex140.c
 * @brief          : Example 140 — LVGL drawing unit on ca_dma2d against the
 *                   LVGL software renderer (F767, H753, U575, H773, F439)
 ******************************************************************************
 * @details
 * One LVGL screen, 240x136 RGB565, drawn twice into RAM - no display:
 *
 *   pass SW     ca_lvgl_dma2d_enable(false): LVGL's software renderer only
 *   pass DMA2D  ca_lvgl_dma2d_enable(true):  fills, plain bitmaps and text
 *               glyphs go to the DMA2D, the rest (radius, slider) stays in
 *               software
 *
 * and the two frames are compared pixel by pixel. The screen mixes what the
 * unit takes (opaque and translucent rectangles, RGB565 and ARGB8888 images,
 * an image at half opacity, labels) with what it must leave alone (a rounded
 * button, a slider), so both paths run in the same frame.
 *
 *   e140 run       both passes, timing (average of N frames), diff, counters
 *   e140 text      text-only screen: software, then the DMA2D with glyph
 *                  thresholds OFF/0/64/100/150/200 px, timing and diff
 *   e140 stats     counters of the DMA2D unit since the last run
 *
 * Opaque operations must match exactly. Blending may differ by rounding:
 * the DMA2D divides by 255 rounding down (RM0410 9.3.6), LVGL uses its own
 * shifts. The run prints how many pixels differ and by how much.
 ******************************************************************************
 */

#include "caos.h"

/* Only where the DMA2D exists (CMSIS): elsewhere the two frames would not
 * even fit (F446 has 128 KB) and there is nothing to compare. */
#if EN_DMA2D && defined(DMA2D)
#include "lvgl.h"

#define W           240U
#if MCU_ID == 439
/* 192 KB of SRAM for the DMA2D (the CCM is out of its reach): 136 lines
 * left a 32 KB heap, short of LVGL's 32 KB pool. The slider falls below. */
#define H           112U
#else
#define H           136U
#endif
#define FRAMES      20U             /* frames per pass for the timing */
#define TEXT_FRAMES 100U            /* text passes: finer than the 1 ms tick */
#define IMG_W       64U
#define IMG_H       40U

/* The two frames, static: on H7 the stacks are in DTCM, out of DMA2D reach.
 * LVGL renders the whole screen straight into one of them (FULL mode). */
static u16 s_fb_sw[W * H] __attribute__((aligned(4)));
static u16 s_fb_hw[W * H] __attribute__((aligned(4)));

static u16 s_img565_px[IMG_W * IMG_H] __attribute__((aligned(4)));
static u32 s_img8888_px[IMG_W * IMG_H];
static lv_image_dsc_t s_img565;
static lv_image_dsc_t s_img8888;

static lv_display_t *s_disp;
static volatile u32  s_req;         /* 1 = run, set by the CLI, done by the GUI task */
static bool          s_has_dma2d;

/* ---------------------------------------------------------------- images */

static void make_images(void)
{
    for (u32 y = 0U; y < IMG_H; y++) {
        for (u32 x = 0U; x < IMG_W; x++) {
            const u32 r = (x * 31U) / (IMG_W - 1U);
            const u32 g = (y * 63U) / (IMG_H - 1U);
            const u32 b = 31U - r;
            s_img565_px[y * IMG_W + x] = (u16)((r << 11) | (g << 5) | b);
            /* ARGB8888: a colour wheel-ish pattern, alpha ramps left to right */
            const u32 a = (x * 255U) / (IMG_W - 1U);
            s_img8888_px[y * IMG_W + x] = (a << 24) | ((255U - (y * 6U)) << 16) |
                                          ((x * 4U) << 8) | (y * 6U);
        }
    }
    s_img565.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_img565.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_img565.header.w      = IMG_W;
    s_img565.header.h      = IMG_H;
    s_img565.header.stride = IMG_W * 2U;
    s_img565.data_size     = sizeof(s_img565_px);
    s_img565.data          = (const u8 *)s_img565_px;

    s_img8888.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_img8888.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    s_img8888.header.w      = IMG_W;
    s_img8888.header.h      = IMG_H;
    s_img8888.header.stride = IMG_W * 4U;
    s_img8888.data_size     = sizeof(s_img8888_px);
    s_img8888.data          = (const u8 *)s_img8888_px;
}

/* ---------------------------------------------------------------- screen */

static lv_obj_t *rect(lv_obj_t *parent, i32 x, i32 y, i32 w, i32 h, u32 rgb, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x203040), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* DMA2D: opaque fills, translucent fills over them */
    (void)rect(scr, 4, 4, 70, 40, 0xC03020, LV_OPA_COVER);
    (void)rect(scr, 40, 20, 70, 40, 0x20C040, LV_OPA_50);
    (void)rect(scr, 80, 8, 50, 50, 0x3060FF, LV_OPA_30);

    /* DMA2D: opaque RGB565 copy, ARGB8888 blend, RGB565 at half opacity */
    lv_obj_t *i1 = lv_image_create(scr);
    lv_image_set_src(i1, &s_img565);
    lv_obj_set_pos(i1, 140, 4);
    lv_obj_t *i2 = lv_image_create(scr);
    lv_image_set_src(i2, &s_img8888);
    lv_obj_set_pos(i2, 160, 30);
    lv_obj_t *i3 = lv_image_create(scr);
    lv_image_set_src(i3, &s_img565);
    lv_obj_set_pos(i3, 4, 70);
    lv_obj_set_style_image_opa(i3, LV_OPA_50, 0);

    /* Software: rounded button, slider; DMA2D: the labels' glyphs */
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_pos(btn, 80, 74);
    lv_obj_set_size(btn, 70, 30);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "caOS");
    lv_obj_center(bl);
    lv_obj_t *lb = lv_label_create(scr);
    lv_label_set_text(lb, "LVGL + DMA2D");
    lv_obj_set_pos(lb, 156, 76);
    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_set_pos(sl, 20, 118);
    lv_obj_set_size(sl, 200, 8);
    lv_slider_set_value(sl, 60, LV_ANIM_OFF);
}

/* ---------------------------------------------------------------- passes */

static void flush_cb(lv_display_t *d, const lv_area_t *a, u8 *px)
{
    LV_UNUSED(a);
    LV_UNUSED(px);
    lv_display_flush_ready(d);      /* FULL mode into RAM: nothing to send */
}

/* n full-screen frames into fb; returns the average in microseconds */
static u32 pass(u16 *fb, bool dma2d, u32 n)
{
    ca_lvgl_dma2d_enable(dma2d);
    lv_display_set_buffers(s_disp, fb, NULL, sizeof(s_fb_sw), LV_DISPLAY_RENDER_MODE_FULL);
    const u32 t0 = ca_os_tick_ms();
    for (u32 i = 0U; i < n; i++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(s_disp);
    }
    return ((ca_os_tick_ms() - t0) * 1000U) / n;
}

static u32 absdiff(u32 a, u32 b) { return a > b ? a - b : b - a; }

/* Pixels that differ between the two frames, the largest delta per channel
 * (RGB565 units) and the index of the first one */
static u32 compare(u32 *dr, u32 *dg, u32 *db, u32 *first)
{
    u32 diff = 0U;
    *dr = *dg = *db = 0U;
    *first = 0xFFFFFFFFU;
    for (u32 i = 0U; i < W * H; i++) {
        const u32 a = s_fb_sw[i], b = s_fb_hw[i];
        if (a == b) continue;
        if (*first == 0xFFFFFFFFU) *first = i;
        diff++;
        const u32 r = absdiff(a >> 11, b >> 11);
        const u32 g = absdiff((a >> 5) & 0x3FU, (b >> 5) & 0x3FU);
        const u32 c = absdiff(a & 0x1FU, b & 0x1FU);
        if (r > *dr) *dr = r;
        if (g > *dg) *dg = g;
        if (c > *db) *db = c;
    }
    return diff;
}

static void run(void)
{
    ca_lvgl_dma2d_stats_t st;
    const u32 us_sw = pass(s_fb_sw, false, FRAMES);
    ca_lvgl_dma2d_stats(NULL, true);
    const u32 us_hw = pass(s_fb_hw, true, FRAMES);
    ca_lvgl_dma2d_stats(&st, false);

    u32 dr, dg, db, first;
    const u32 diff = compare(&dr, &dg, &db, &first);

    ca_stdout_printf_n("E140 frame %ux%u RGB565, %u frames per pass, %u MHz core",
                       W, H, FRAMES, SystemCoreClock / 1000000U);
    ca_stdout_printf_n("E140 time sw %u us, dma2d %u us per frame", us_sw, us_hw);
    ca_stdout_printf_n("E140 dma2d fills %u fill_blends %u copies %u converts %u img_blends %u fallback %u err %02X",
                       st.fills / FRAMES, st.fill_blends / FRAMES, st.copies / FRAMES,
                       st.converts / FRAMES, st.img_blends / FRAMES, st.fallback_sw, st.last_error);
    if (diff == 0U) {
        ca_stdout_printf_n("E140 diff 0 px: frames identical");
    } else {
        ca_stdout_printf_n("E140 diff %u px of %u, max delta r%u g%u b%u (RGB565 units), first at %u,%u",
                           diff, W * H, dr, dg, db, first % W, first / W);
    }

    /* Where only opaque DMA2D work lands (nothing blended over it): the red
     * rectangle left of the green one, the opaque image left of and above
     * the ARGB8888 one. Must be exact. */
    static const struct { u16 x0, y0, x1, y1; } opaque[] = {
        {   4U,  4U,  39U, 43U },      /* red fill */
        { 140U,  4U, 159U, 43U },      /* RGB565 copy, left of the ARGB image */
        { 160U,  4U, 203U, 29U },      /* RGB565 copy, above it */
    };
    u32 odiff = 0U;
    for (u32 k = 0U; k < sizeof(opaque) / sizeof(opaque[0]); k++)
        for (u32 y = opaque[k].y0; y <= opaque[k].y1; y++)
            for (u32 x = opaque[k].x0; x <= opaque[k].x1; x++)
                if (s_fb_sw[y * W + x] != s_fb_hw[y * W + x]) odiff++;

    const u32 ops = st.fills + st.fill_blends + st.copies + st.converts + st.img_blends;
    const bool pass_ok = ops > 0U && st.fallback_sw == 0U && odiff == 0U &&
                         dr <= 2U && dg <= 2U && db <= 2U;
    ca_stdout_printf_n("E140 opaque regions %u px differ", odiff);
    ca_stdout_printf_n("E140 RESULT %s (dma2d ops, no fallback, opaque exact, blend within 2)",
                       pass_ok ? "PASS" : "FAIL");
}

/* ---------------------------------------------------------------- text */

static lv_obj_t *s_scr_main;
static lv_obj_t *s_scr_text;

static void text_line(lv_obj_t *scr, const lv_font_t *font, i32 y, u32 rgb, lv_opa_t opa, const char *s)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(l, (i32)W);
    lv_obj_set_pos(l, 2, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    lv_label_set_text(l, s);
}

/* Only text: two lines in Montserrat 20, four in 14, one translucent */
static void build_text_screen(void)
{
    s_scr_text = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr_text);
    lv_obj_set_style_bg_color(s_scr_text, lv_color_hex(0x102030), 0);
    lv_obj_set_style_bg_opa(s_scr_text, LV_OPA_COVER, 0);
    text_line(s_scr_text, &lv_font_montserrat_20,  0, 0xFFFFFF, LV_OPA_COVER, "caOS LVGL text 0123456789");
    text_line(s_scr_text, &lv_font_montserrat_20, 23, 0xFFD040, LV_OPA_COVER, "Glyphs on the DMA2D: A8");
    text_line(s_scr_text, &lv_font_montserrat_14, 48, 0xE0E0E0, LV_OPA_COVER, "The quick brown fox jumps over the lazy dog.");
    text_line(s_scr_text, &lv_font_montserrat_14, 64, 0x80FF80, LV_OPA_COVER, "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG!");
    text_line(s_scr_text, &lv_font_montserrat_14, 80, 0x40C0FF, LV_OPA_50,    "translucent 50%: abcdefghijklmnopqrstuvwxyz");
    text_line(s_scr_text, &lv_font_montserrat_14, 96, 0xFF8080, LV_OPA_COVER, "{[(<0123456789 +-*/=%&$#@!?>)]}");
}

/* min_px for each DMA2D pass of `e140 text` (glyphs polled) */
static const u32 s_text_min[] = {
    CA_LVGL_TEXT_OFF,           /* reference: background on the DMA2D, text on the CPU */
    0U,                         /* every glyph: the default */
    64U, 100U, 150U, 200U,
};

static void run_text(void)
{
    ca_lvgl_dma2d_stats_t st;
    bool all_ok = true;

    lv_screen_load(s_scr_text);
    const u32 us_sw = pass(s_fb_sw, false, TEXT_FRAMES);
    ca_stdout_printf_n("E140 text %ux%u, %u frames per pass, %u MHz core: sw %u us per frame",
                       W, H, TEXT_FRAMES, SystemCoreClock / 1000000U, us_sw);

    u32 us_ref = us_sw;             /* % against the OFF row: text alone */
    for (u32 k = 0U; k < sizeof(s_text_min) / sizeof(s_text_min[0]); k++) {
        ca_lvgl_dma2d_text(s_text_min[k]);
        ca_lvgl_dma2d_stats(NULL, true);
        const u32 us = pass(s_fb_hw, true, TEXT_FRAMES);
        ca_lvgl_dma2d_stats(&st, false);
        u32 dr, dg, db, first;
        const u32 diff = compare(&dr, &dg, &db, &first);
        const bool off = s_text_min[k] == CA_LVGL_TEXT_OFF;
        const bool ok = st.fallback_sw == 0U && dr <= 2U && dg <= 2U && db <= 2U
                     && (off || st.glyphs > 0U);
        if (!ok) all_ok = false;
        if (off) us_ref = us;
        ca_stdout_printf_n("E140 text %s %u: %u us (%d%%) dma2d %u cpu %u fb %u diff %u px max r%u g%u b%u %s",
                           off ? "OFF" : "min", off ? 0U : s_text_min[k], us,
                           (int)(((i32)us - (i32)us_ref) * 100 / (i32)(us_ref ? us_ref : 1U)),
                           st.glyphs / TEXT_FRAMES, st.glyphs_sw / TEXT_FRAMES,
                           st.fallback_sw, diff, dr, dg, db, ok ? "ok" : "BAD");
    }
    ca_lvgl_dma2d_text(0U);                 /* back to the default */
    lv_screen_load(s_scr_main);
    ca_stdout_printf_n("E140 TEXT RESULT %s (glyphs on dma2d, no fallback, within 2)",
                       all_ok ? "PASS" : "FAIL");
}

/* ---------------------------------------------------------------- GUI task */

static u32 task_gui(void *arg)
{
    (void)arg;
    lv_init();
    lv_tick_set_cb(ca_os_tick_ms);
    s_disp = lv_display_create(W, H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_fb_sw, NULL, sizeof(s_fb_sw), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    s_has_dma2d = ca_lvgl_dma2d_init();
    make_images();
    build_screen();
    s_scr_main = lv_screen_active();
    build_text_screen();
    lv_refr_now(s_disp);
    ca_stdout_printf_n("E140 ready: LVGL %u.%u.%u, DMA2D unit %s, heap free %u",
                       (u32)LVGL_VERSION_MAJOR, (u32)LVGL_VERSION_MINOR, (u32)LVGL_VERSION_PATCH,
                       s_has_dma2d ? "on" : "NOT AVAILABLE", ca_memory_get_avail_memory());

    for (;;) {
        if (s_req != 0U) {
            if (!s_has_dma2d)  ca_stdout_printf_n("E140 run: no DMA2D on this part");
            else if (s_req == 2U) run_text();
            else               run();
            s_req = 0U;
        }
        lv_timer_handler();
        ca_os_sleep(10U);
    }
    return 0U;
}

/* ---------------------------------------------------------------- CLI */

#if ENABLE_CLI
/* e140 — the line arrives without the command word */
static bool e140_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "run", 3) == 0) {
        /* LVGL belongs to the GUI task: ask it, it prints the result */
        s_req = 1U;
        return true;
    }
    if (ca_strncmp(p, "text", 4) == 0) {
        s_req = 2U;                 /* text screen, threshold sweep */
        return true;
    }
    if (ca_strncmp(p, "stats", 5) == 0) {
        ca_lvgl_dma2d_stats_t st;
        ca_lvgl_dma2d_stats(&st, false);
        CLI_PRINTF_N("E140 stats fills %u fill_blends %u copies %u converts %u img_blends %u fallback %u err %02X",
                     st.fills, st.fill_blends, st.copies, st.converts, st.img_blends,
                     st.fallback_sw, st.last_error);
        return true;
    }
    return false;                   /* cli prints the help line */
}
static const ca_cli_cmd_t s_e140_cmds[] = {
    { "e140", "e140      : run | text | stats", { e140_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_e140_table = {
    "e140", s_e140_cmds, (u16)(sizeof(s_e140_cmds) / sizeof(s_e140_cmds[0])), NULL
};
#endif

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
        ca_cli_register_table(&s_e140_table);
    )
}

void ca_os_main(void)
{
    /* LVGL's software renderer nests deep: at least 8 KB of stack */
    ca_stack_size_t stk = STACK_LARGE;
    if ((u32)stk < 8192U) stk = STACK_KERNEL;
    ca_os_job("gui", CA_TH_LEVEL3, task_gui, NULL, stk, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* EN_DMA2D && DMA2D */

PRIVATE_STRING(why, "ex140: no DMA2D on this part (F439, F767, H753, H773 and U575 have it)");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_DMA2D && DMA2D */
