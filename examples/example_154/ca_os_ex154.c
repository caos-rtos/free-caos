// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex154.c
 * @brief          : Example 154 — MicroPython on the 1.8" ST7735 TFT
 ******************************************************************************
 * @details
 * The REPL of example 145 with the display of example 152 in Python:
 *
 *  - machine.ST7735() opens the panel with the board's wiring, which this
 *    program hands to Python (ca_python_set_lcd, from lcd_pins.h): no pin
 *    table in the script. Landscape, 160x128;
 *  - framebuf draws text, lines and rectangles in a bytearray, blit() sends
 *    it to the glass over the SPI DMA;
 *  - caos.FB('lcd') is a caOS framebuffer of the panel's size, published by
 *    this program (ca_python_publish_fb): Python draws in it, show() sends
 *    only the 16x16 tiles that changed. The same object a VNC service would
 *    show (EN_VNC_SERVICE): what Python draws is what the viewer sees.
 *
 *   >>> import machine, framebuf
 *   >>> lcd = machine.ST7735()
 *   >>> buf = bytearray(160 * 128 * 2)
 *   >>> fb = framebuf.FrameBuffer(buf, 160, 128, framebuf.RGB565)
 *   >>> fb.fill(0); fb.text('caOS', 8, 8, lcd.WHITE); lcd.blit(buf)
 *   >>> import caos
 *   >>> c = caos.FB('lcd'); c.fill_rect(10, 40, 50, 20, lcd.RED); lcd.show(c)
 *
 * HARDWARE: the wiring of example 152 (lcd_pins.h), on every board it lists;
 * `lcd wire` prints it for the board it runs on.
 *
 * CLI:
 *   py           MicroPython REPL (Ctrl-D on an empty line exits)
 *   lcd wire     the wiring of this board
 ******************************************************************************
 */

#include "caos.h"
#include "../example_152/lcd_pins.h"

#if EN_PYTHON && ENABLE_CLI && EN_SPI_DMA && EN_DMA && EN_TIMER && EN_FB && LCD_PINS_OK

#define SCR_W   160U
#define SCR_H   128U

/* landscape: rotation 1, as example 153 */
static const ca_st7735_cfg_t s_lcd_cfg = LCD_CFG(1U);

static ca_fb_t s_fb = CA_FB_NONE;

PRIVATE_STRING(nofb, "ex154: no heap for the 160x128 framebuffer, caos.FB('lcd') missing");

/* Immediate: prints a table, runs in cli */
static bool lcd_cmd(c_str *args)
{
    const char *p = (*args)->data;
    while (*p == ' ') p++;
    if (ca_strncmp(p, "wire", 4) == 0) {
        lcd_wire_print();
        return true;
    }
    return false;                   /* cli prints the help line */
}

static const ca_cli_cmd_t s_e154_cmds[] = {
    { "lcd", "lcd       : wire (the TFT wiring of this board; the panel is Python's: machine.ST7735())",
      { lcd_cmd, (ca_stack_size_t)0 } },
};

static ca_cli_table_t s_e154_table = {
    "lcd", s_e154_cmds, (u16)(sizeof(s_e154_cmds) / sizeof(s_e154_cmds[0])), NULL
};

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
        ca_cli_register_table(&s_e154_table);
    )
    ca_dma_init();
    ca_python_set_lcd(&s_lcd_cfg);
    s_fb = ca_fb_create(SCR_W, SCR_H, CA_FB_FMT_RGB565, 1U);
    if (s_fb == CA_FB_NONE || !ca_python_publish_fb("lcd", s_fb)) PUT_N(&priv_nofb);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else

PRIVATE_STRING(m154a, "=== Example 154: MicroPython on the ST7735 TFT ===");
PRIVATE_STRING(m154b, "needs EN_PYTHON, ENABLE_CLI, EN_SPI_DMA, EN_DMA, EN_TIMER, EN_FB and a board in lcd_pins.h");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m154a);
    PUT_N(&priv_m154b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
