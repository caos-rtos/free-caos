// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex152.c
 * @brief          : Example 152 — 1.8" TFT 128x160 (ST7735) on SPI1 with DMA
 ******************************************************************************
 * @details
 * Drives a 1.8" 128x160 RGB565 TFT module with an ST7735 controller (st7735.h)
 * and draws a test pattern made to recognise the module variant at a glance:
 *
 *   - a 1-pixel white frame on the screen edge: if a side is missing, or a
 *     line of noise shows on the opposite side, the column/row offset is wrong
 *     ("lcd var 2 1 ..." for a green-tab glass, "0 0" for red/black tab);
 *   - corner squares RED top-left, GREEN top-right, BLUE bottom-left, WHITE
 *     bottom-right: if red and blue are swapped the colour order is wrong
 *     ("lcd var .. .. 1 .." / "0"), if they are in other corners the rotation;
 *   - three bars R G B and a grey ramp: washed-out or negative colours mean
 *     the glass wants the inversion on ("lcd var .. .. .. 1 ..").
 *
 * HARDWARE — every Nucleo-144 of the bench (U575 F446 F439 F767 H753 H773
 * H563): the pins per board are in lcd_pins.h, and `lcd wire` prints the
 * wiring of the board it runs on. CS = PD14 (CN7-16), BL = PD15 TIM4_CH4
 * (CN7-18) everywhere; SCK/SDA on PA5/PA7 or on the alternate PB3/PB5 where
 * PA7 is the Ethernet; DC/RES on D8/D7.
 *
 *   SPI1 at APB2 / 8: 10 MHz on the U575 (APB2 80 MHz), one full frame
 *   (40 KB) in ~34 ms. The ST7735 write clock max is ~15 MHz.
 *
 * CLI ("lcd", runs in cli_cmd):
 *   lcd wire                          print the wiring of this board
 *   lcd test                          redraw the test pattern
 *   lcd var <col> <row> <bgr> <inv> <rot>   change the variant and redraw
 *   lcd bl <0..1000>                  backlight
 *   lcd fill <rrggbb>                 whole screen in one colour (hex)
 *   lcd speed                         20 full-frame blits: ms per frame, fps
 *   lcd info                          current variant and size
 */
#include "caos.h"
#include "lcd_pins.h"

#if EN_SPI_DMA && EN_DMA && EN_TIMER && LCD_PINS_OK

static const ca_st7735_cfg_t s_lcd_cfg = LCD_CFG(0U);

/* the frame drawn in RAM and sent with ca_st7735_blit() */
static u16  s_fb[ST7735_WIDTH * ST7735_HEIGHT];
static bool s_ok;
static u8   s_col = 0U, s_row = 0U, s_rot = 0U;
static bool s_bgr = false, s_inv = false;

static void fb_rect(u16 w, u16 x, u16 y, u16 rw, u16 rh, u16 c)
{
    for (u16 j = y; j < (u16)(y + rh); j++)
        for (u16 i = x; i < (u16)(x + rw); i++)
            s_fb[(u32)j * w + i] = c;
}

static bool draw_pattern(void)
{
    const u16 w = ca_st7735_width(), h = ca_st7735_height();
    const u16 sq = 16U;
    fb_rect(w, 0U, 0U, w, h, CA_RGB565_BLACK);
    /* three bars R G B in the middle */
    const u16 bw = (u16)((w - 8U) / 3U);
    const u16 by = (u16)(sq + 6U), bh = (u16)(h - 2U * sq - 28U);
    fb_rect(w, 4U,                  by, bw, bh, CA_RGB565_RED);
    fb_rect(w, (u16)(4U + bw),      by, bw, bh, CA_RGB565_GREEN);
    fb_rect(w, (u16)(4U + 2U * bw), by, bw, bh, CA_RGB565_BLUE);
    /* grey ramp under the bars */
    const u16 ry = (u16)(by + bh + 4U);
    for (u16 i = 4U; i < (u16)(w - 4U); i++) {
        const u16 v = (u16)(((u32)(i - 4U) * 255U) / (u32)(w - 9U));
        fb_rect(w, i, ry, 1U, 8U, CA_RGB565(v, v, v));
    }
    /* corner squares: R top-left, G top-right, B bottom-left, W bottom-right */
    fb_rect(w, 2U,              2U,              sq, sq, CA_RGB565_RED);
    fb_rect(w, (u16)(w - sq - 2U), 2U,           sq, sq, CA_RGB565_GREEN);
    fb_rect(w, 2U,              (u16)(h - sq - 2U), sq, sq, CA_RGB565_BLUE);
    fb_rect(w, (u16)(w - sq - 2U), (u16)(h - sq - 2U), sq, sq, CA_RGB565_WHITE);
    /* 1-pixel frame on the very edge */
    fb_rect(w, 0U, 0U, w, 1U, CA_RGB565_WHITE);
    fb_rect(w, 0U, (u16)(h - 1U), w, 1U, CA_RGB565_WHITE);
    fb_rect(w, 0U, 0U, 1U, h, CA_RGB565_WHITE);
    fb_rect(w, (u16)(w - 1U), 0U, 1U, h, CA_RGB565_WHITE);
    return ca_st7735_blit(0U, 0U, w, h, s_fb, 0U);
}

static u32 task_lcd(void *arg)
{
    (void)arg;
    /* SPI+DMA brought up in a task, as ex64: the init sleeps, and the first
     * DMA transfer pre-scheduler failed there */
    ca_os_sleep(200U);
    s_ok = ca_st7735_init(&s_lcd_cfg);
    EVAL_STDOUT(
        ca_stdout_put_n("\r\n=== Example 152: TFT 1.8\" 128x160 ST7735 ===");
        if (s_ok)
            ca_stdout_printf_n("EX152 lcd ready %ux%u, SPI1 DMA, backlight TIM4_CH4",
                               (u32)ca_st7735_width(), (u32)ca_st7735_height());
        else
            ca_stdout_put_n("EX152 lcd init FAILED - 'lcd wire' prints the wiring of this board");
    )
    if (s_ok && draw_pattern()) {
        EVAL_STDOUT( ca_stdout_put_n("EX152 test pattern drawn - 'lcd var' to fix offsets/colours"); )
    }
    return 0U;
}

#if ENABLE_CLI
/* next unsigned number in p (decimal, or hex with hex = true); false if none */
static bool next_num(const char **p, u32 *v, bool hex)
{
    const char *s = *p;
    while (*s == ' ') s++;
    u32 n = 0U, digits = 0U;
    for (;; s++, digits++) {
        u32 d;
        if (*s >= '0' && *s <= '9') d = (u32)(*s - '0');
        else if (hex && *s >= 'a' && *s <= 'f') d = (u32)(*s - 'a' + 10);
        else if (hex && *s >= 'A' && *s <= 'F') d = (u32)(*s - 'A' + 10);
        else break;
        n = n * (hex ? 16U : 10U) + d;
    }
    *p = s;
    *v = n;
    return digits != 0U;
}

static bool lcd_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "wire", 4) == 0) { lcd_wire_print(); return true; }
    if (!s_ok) { CLI_PUT_N("lcd: not initialised ('lcd wire' for the wiring)"); return true; }
    if (ca_strncmp(p, "test", 4) == 0) {
        CLI_PUT_N(draw_pattern() ? "lcd: pattern drawn" : "lcd: blit FAILED");
        return true;
    }
    if (ca_strncmp(p, "var", 3) == 0) {
        const char *q = p + 3;
        u32 c, r, b, i, o;
        if (!next_num(&q, &c, false) || !next_num(&q, &r, false) || !next_num(&q, &b, false) ||
            !next_num(&q, &i, false) || !next_num(&q, &o, false) || o > 3U) {
            CLI_PUT_N("lcd var <col> <row> <bgr 0|1> <inv 0|1> <rot 0..3>");
            return true;
        }
        s_col = (u8)c; s_row = (u8)r; s_bgr = (b != 0U); s_inv = (i != 0U); s_rot = (u8)o;
        const bool ok = ca_st7735_set_variant(s_col, s_row, s_bgr, s_inv, s_rot) && draw_pattern();
        CLI_PRINTF_N("lcd: col %u row %u bgr %u inv %u rot %u -> %s", (u32)s_col, (u32)s_row,
                     (u32)s_bgr, (u32)s_inv, (u32)s_rot, ok ? "ok" : "FAILED");
        return true;
    }
    if (ca_strncmp(p, "bl", 2) == 0) {
        const char *q = p + 2;
        u32 v;
        if (!next_num(&q, &v, false) || v > 1000U) { CLI_PUT_N("lcd bl <0..1000>"); return true; }
        ca_st7735_backlight((u16)v);
        CLI_PRINTF_N("lcd: backlight %u/1000", v);
        return true;
    }
    if (ca_strncmp(p, "fill", 4) == 0) {
        const char *q = p + 4;
        u32 rgb;
        if (!next_num(&q, &rgb, true)) { CLI_PUT_N("lcd fill <rrggbb>"); return true; }
        const u16 c = CA_RGB565((rgb >> 16) & 0xFFU, (rgb >> 8) & 0xFFU, rgb & 0xFFU);
        const bool ok = ca_st7735_fill(0U, 0U, ca_st7735_width(), ca_st7735_height(), c);
        CLI_PRINTF_N("lcd: fill %06x (rgb565 %04x) %s", rgb, (u32)c, ok ? "ok" : "FAILED");
        return true;
    }
    if (ca_strncmp(p, "speed", 5) == 0) {
        const u16 w = ca_st7735_width(), h = ca_st7735_height();
        const u32 n = 20U;
        const u32 t0 = ca_os_tick_ms();
        bool ok = true;
        for (u32 k = 0U; ok && k < n; k++) ok = ca_st7735_blit(0U, 0U, w, h, s_fb, 0U);
        u32 ms = ca_os_tick_ms() - t0;
        if (ms == 0U) ms = 1U;
        CLI_PRINTF_N("lcd speed: %u frames %ux%u in %u ms = %u ms/frame, %u fps %s",
                     n, (u32)w, (u32)h, ms, ms / n, (n * 1000U) / ms, ok ? "" : "(blit FAILED)");
        return true;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("lcd: %ux%u  col %u row %u bgr %u inv %u rot %u", (u32)ca_st7735_width(),
                     (u32)ca_st7735_height(), (u32)s_col, (u32)s_row, (u32)s_bgr, (u32)s_inv,
                     (u32)s_rot);
        return true;
    }
    return false;                   /* cli prints the help line */
}

static const ca_cli_cmd_t s_e152_cmds[] = {
    { "lcd", "lcd       : wire | test | var <col> <row> <bgr> <inv> <rot> | bl <0..1000> | fill <rrggbb> | speed | info",
      { lcd_cmd, STACK_MEDIUM } },
};

static ca_cli_table_t s_e152_table = {
    "lcd", s_e152_cmds, (u16)(sizeof(s_e152_cmds) / sizeof(s_e152_cmds[0])), NULL
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
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    ca_dma_init();
    EVAL_CLI( ca_cli_register_table(&s_e152_table); )
}

void ca_os_main(void)
{
    ca_os_job("lcd", CA_TH_LEVEL4, task_lcd, NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m152a, "=== Example 152: TFT 1.8\" 128x160 ST7735 ===");
PRIVATE_STRING(m152b, "needs EN_SPI_DMA, EN_DMA, EN_TIMER and a board listed in lcd_pins.h");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m152a);
    PUT_N(&priv_m152b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
