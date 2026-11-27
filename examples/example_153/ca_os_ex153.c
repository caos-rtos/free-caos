// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex153.c
 * @brief          : Example 153 — LVGL on the 1.8" ST7735 TFT (160x128)
 ******************************************************************************
 * @details
 * LVGL draws into a caOS framebuffer (RGB565, 160x128 landscape) with
 * ca_lvgl_start(), as for the VNC examples; a "show" task sends the changed
 * 16x16 tiles to the panel every 20 ms with ca_st7735_show_fb() over the
 * SPI DMA. The fb belongs to whoever shows it: here the panel, there VNC.
 *
 * The module has no touch: the screen moves by itself (spinner, a bar that
 * sweeps, uptime) and the CLI changes it through ca_lvgl_call(), which runs
 * the change in the GUI task (LVGL is not thread-safe).
 *
 * HARDWARE: the wiring of example 152 (lcd_pins.h), on every board it lists
 * but the F446 (128 KB of RAM: fb + LVGL pool + draw buffer do not fit);
 * `e153 wire` prints it for the board it runs on. Without a DMA2D (H563)
 * LVGL draws in software.
 *
 * CLI ("e153"):
 *   e153 wire           the wiring of this board
 *   e153 text <words>   the message line
 *   e153 bl <0..1000>   backlight
 *   e153 stat           tiles sent to the panel, show cycles, LVGL DMA2D units
 */
#include "caos.h"
#include "../example_152/lcd_pins.h"

#if EN_SPI_DMA && EN_DMA && EN_TIMER && EN_FB && LCD_PINS_OK && MCU_ID != 446

#include "lvgl.h"

#define SCR_W   160U
#define SCR_H   128U

static const ca_st7735_cfg_t s_lcd_cfg = LCD_CFG(1U);    /* landscape 160x128 */

static ca_fb_t   s_fb = CA_FB_NONE;
static volatile u32 s_shows, s_show_fail;
static lv_obj_t *s_msg, *s_up, *s_bar;
static char      s_text[48];

/* ---- UI, in the GUI task ------------------------------------------------- */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    static u32 v;
    v = (v + 5U) % 105U;
    lv_bar_set_value(s_bar, (int32_t)(v > 100U ? 100U : v), LV_ANIM_ON);
    lv_label_set_text_fmt(s_up, "up %us", (unsigned)(ca_os_tick_ms() / 1000U));
}

static void build_ui(void *arg)
{
    (void)arg;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x001830), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "caOS + LVGL");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *sp = lv_spinner_create(scr);
    lv_obj_set_size(sp, 44, 44);
    lv_obj_align(sp, LV_ALIGN_LEFT_MID, 8, -4);

    s_up = lv_label_create(scr);
    lv_obj_set_style_text_color(s_up, lv_color_hex(0x80FF80), 0);
    lv_label_set_text(s_up, "up 0s");
    lv_obj_align(s_up, LV_ALIGN_RIGHT_MID, -8, -14);

    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 80, 12);
    lv_obj_align(s_bar, LV_ALIGN_RIGHT_MID, -8, 8);

    s_msg = lv_label_create(scr);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(0xFFD040), 0);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_msg, SCR_W - 8);
    lv_label_set_text(s_msg, "ST7735 128x160 on SPI1 DMA");
    lv_obj_align(s_msg, LV_ALIGN_BOTTOM_MID, 0, -4);

    (void)lv_timer_create(tick_cb, 200, NULL);
    ca_stdout_put_n("EX153 GUI ready");
}

/* ---- show task: init the panel, then copy the changed tiles ------------- */
static u32 task_show(void *arg)
{
    (void)arg;
    ca_os_sleep(200U);                  /* SPI+DMA in a task, as ex64/ex152 */
    if (!ca_st7735_init(&s_lcd_cfg)) {
        ca_stdout_put_n("EX153 lcd init FAILED - 'e153 wire' prints the wiring of this board");
        return 1U;
    }
    ca_stdout_printf_n("EX153 lcd ready %ux%u", (u32)ca_st7735_width(), (u32)ca_st7735_height());
    const ca_lvgl_cfg_t lc = { .fb = s_fb, .buf_lines = 16U, .extra_heap = 0U,
                               .use_dma2d = true, .cursor = false,
                               .setup = build_ui, .setup_arg = NULL };
    if (!ca_lvgl_start(&lc)) {
        ca_stdout_put_n("EX153 LVGL did not start");
        return 1U;
    }
    for (;;) {
        if (ca_st7735_show_fb(s_fb)) s_shows++;
        else                          s_show_fail++;
        ca_os_sleep(20U);
    }
}

/* ---- CLI ------------------------------------------------------------------ */
#if ENABLE_CLI
static void set_text(void *arg)
{
    (void)arg;
    lv_label_set_text(s_msg, s_text);
}

static bool e153_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "wire", 4) == 0) { lcd_wire_print(); return true; }
    if (ca_strncmp(p, "text", 4) == 0) {
        p += 4;
        while (*p == ' ') p++;
        u32 n = 0U;
        while (p[n] != '\0' && n < sizeof(s_text) - 1U) { s_text[n] = p[n]; n++; }
        s_text[n] = '\0';
        CLI_PRINTF_N("e153: text \"%s\" %s", s_text, ca_lvgl_call(set_text, NULL) ? "queued" : "FAILED (queue full)");
        return true;
    }
    if (ca_strncmp(p, "bl", 2) == 0) {
        p += 2;
        while (*p == ' ') p++;          /* ca_strtol stops at a leading space: 0 */
        u32 v = (u32)ca_strtol(p, 10U);
        if (v > 1000U) v = 1000U;
        ca_st7735_backlight((u16)v);
        CLI_PRINTF_N("e153: backlight %u/1000", v);
        return true;
    }
    if (ca_strncmp(p, "stat", 4) == 0) {
        CLI_PRINTF_N("e153: show cycles %u, failed %u", s_shows, s_show_fail);
#if EN_DMA2D
        ca_lvgl_dma2d_stats_t st;
        ca_lvgl_dma2d_stats(&st, false);
        CLI_PRINTF_N("e153: dma2d fills %u copies %u blends %u glyphs %u fallback %u",
                     st.fills, st.copies, st.img_blends + st.fill_blends, st.glyphs, st.fallback_sw);
#endif
        return true;
    }
    return false;
}

static const ca_cli_cmd_t s_e153_cmds[] = {
    { "e153", "e153      : wire | text <words> | bl <0..1000> | stat", { e153_cmd, 0U } },
};
static ca_cli_table_t s_e153_table = {
    "e153", s_e153_cmds, (u16)(sizeof(s_e153_cmds) / sizeof(s_e153_cmds[0])), NULL
};
#endif

PRIVATE_STRING(nofb, "ex153: no memory for the framebuffer");

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
        ca_cli_register_table(&s_e153_table);
    )
    ca_dma_init();
    s_fb = ca_fb_create(SCR_W, SCR_H, CA_FB_FMT_RGB565, 1U);
    if (s_fb == CA_FB_NONE) PUT_N(&priv_nofb);
}

void ca_os_main(void)
{
    if (s_fb != CA_FB_NONE)
        ca_os_job("show", CA_TH_LEVEL3, task_show, NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m153a, "=== Example 153: LVGL on the ST7735 TFT ===");
PRIVATE_STRING(m153b, "needs EN_SPI_DMA, EN_DMA, EN_TIMER, EN_FB and a board in lcd_pins.h (not the F446: RAM)");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m153a);
    PUT_N(&priv_m153b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
