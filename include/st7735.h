// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    st7735.h
 * @brief   ST7735 TFT controller (1.8" 128x160 RGB565 modules) over SPI
 ******************************************************************************
 * The panel is write-only SPI (SCK, SDA = MOSI, CS, DC, RES; no MISO) with its
 * own frame memory: the driver sends the controller init, opens a window
 * (CASET/RASET) and streams RGB565 pixels with the SPI DMA. DC selects
 * command (low) or data (high); CS is held low for a whole command + data
 * sequence. One panel per firmware.
 *
 * The many module variants differ in three things only, all in the config:
 *   col_off/row_off  where the 128x160 glass starts in the 132x162 GRAM
 *                    ("green tab": 2/1, "red/black tab": 0/0)
 *   bgr              colour order of the glass (MADCTL bit 3)
 *   invert           some glasses need the inversion on (INVON)
 * and can be changed at run time (ca_st7735_set_variant) to find them.
 *
 * Pixels in memory are RGB565 little-endian (as a caOS fb or LVGL buffer);
 * the controller wants them big-endian: ca_st7735_blit() swaps them through
 * a small buffer of its own, ca_st7735_write_raw() sends bytes as they are.
 *
 * Backlight: a timer PWM channel (bl_tim != 0, duty 0..1000) or a plain GPIO
 * (bl_tim = 0: on/off). Needs EN_SPI_DMA (it brings the SPI); the PWM needs EN_TIMER.
 * No EN_ flag of its own: unused code is dropped by the linker.
 * Task context only (the SPI DMA waits on a semaphore).
 */
#ifndef ST7735_H
#define ST7735_H

#include "caos.h"

#if EN_SPI_DMA

/* Glass size in portrait (rotation 0) */
#define ST7735_WIDTH    128U
#define ST7735_HEIGHT   160U

/* RGB565 helpers */
#define CA_RGB565(r, g, b)  ((u16)((((u16)(r) & 0xF8U) << 8) | (((u16)(g) & 0xFCU) << 3) | ((u16)(b) >> 3)))
#define CA_RGB565_BLACK     0x0000U
#define CA_RGB565_WHITE     0xFFFFU
#define CA_RGB565_RED       0xF800U
#define CA_RGB565_GREEN     0x07E0U
#define CA_RGB565_BLUE      0x001FU

typedef struct {
    /* SPI: master, mode 0, write only; ca_spi_init_dma() is done here */
    u8              spi_num;
    ca_spi_br_t     br;             /* ST7735 write clock max ~15 MHz */
    bool            alternate_gpio; /* SPI pin mapping, as ca_spi_config_t */
    ca_gpio_port_t  cs_port;   ca_gpio_pin_t cs_pin;
    ca_gpio_port_t  dc_port;   ca_gpio_pin_t dc_pin;
    ca_gpio_port_t  rst_port;  ca_gpio_pin_t rst_pin;
    /* backlight: PWM when bl_tim != 0 (pin set to bl_af), else GPIO on/off */
    u8              bl_tim;    u8 bl_ch;   u8 bl_af;
    ca_gpio_port_t  bl_port;   ca_gpio_pin_t bl_pin;
    /* variant of the glass */
    u8              col_off;   u8 row_off;
    bool            bgr;
    bool            invert;
    u8              rotation;       /* 0..3, 90 degrees steps */
} ca_st7735_cfg_t;

/* Reset, controller init, display on, backlight at 100%. Can be called
 * again (a retry, a new MicroPython VM) on the same SPI: the SPI DMA is bound
 * once and keeps the first call's clock and CS; another spi_num is refused. */
bool ca_st7735_init(const ca_st7735_cfg_t *cfg);

/* Current drawable size (swapped by rotation 1 and 3). */
u16  ca_st7735_width(void);
u16  ca_st7735_height(void);

/* Change the variant at run time: offsets, colour order, inversion, rotation. */
bool ca_st7735_set_variant(u8 col_off, u8 row_off, bool bgr, bool invert, u8 rotation);

/* Fill a rectangle with one colour (clipped to the screen). */
bool ca_st7735_fill(u16 x, u16 y, u16 w, u16 h, u16 rgb565);

/* Copy a w x h block of RGB565 little-endian pixels to (x, y). stride =
 * pixels from one source line to the next (0 = w): a block cut out of a
 * larger image passes the image's width. */
bool ca_st7735_blit(u16 x, u16 y, u16 w, u16 h, const u16 *pixels, u16 stride);

/* Open a window, then send bytes already in controller order (big-endian
 * RGB565): the window may be filled by several write_raw calls. */
bool ca_st7735_window(u16 x, u16 y, u16 w, u16 h);
bool ca_st7735_write_raw(const u8 *bytes, u32 len);

/* Backlight 0..1000 (GPIO: 0 = off, anything else = on). */
void ca_st7735_backlight(u16 permille);

/* Display on/off (the frame memory is kept). */
bool ca_st7735_display(bool on);

#if EN_FB
/* Show a caOS framebuffer (RGB565, the panel's size for the rotation): takes
 * its dirty map and sends only the changed 16x16 tiles, a run of adjacent
 * tiles per DMA window. Call it periodically from the task that shows the
 * frame (LVGL draws into the fb, this copies it to the glass). true: nothing
 * to send or all sent. */
bool ca_st7735_show_fb(ca_fb_t fb);
#endif

#endif /* EN_SPI_DMA */
#endif /* ST7735_H */
