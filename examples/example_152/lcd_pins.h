// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    lcd_pins.h
 * @brief   1.8" ST7735 TFT wiring per Nucleo-144 (examples 152 and 153)
 ******************************************************************************
 * One place for the pins the examples program AND for the table `wire`
 * prints: the two come from the same block, so the printed wiring is the
 * one the driver really uses.
 *
 * Common to every board: CS = PD14 (CN7-16, D10), BL = PD15 = TIM4_CH4 AF2
 * (CN7-18, D9), GND CN7-8, VCC 3.3 V on CN8-7. What changes:
 *   - SPI1 on PA5/PA7 where PA7 is free (U575, F446), on the alternate map
 *     PB3/PB5 where PA7 is the Ethernet RMII_CRS_DV (F439 F767 H753 H773 H563);
 *   - DC and RES are D8 and D7, which are different MCU pins per board;
 *   - H563: PB3 and PB5 do not reach CN7 (SB33 OFF / SB17 OFF from the
 *     factory): SCK and SDA go to the morpho CN12.
 * Sources: UM2861 Table 15 (U575), UM1974 Tables 17-19 (F439 F446 F767),
 * UM2407 Table 17 (H753), UM3115 Table 21 + SB33/SB39 (H563), UM3276
 * Table 13 + SB45 (H773).
 */
#ifndef LCD_PINS_H
#define LCD_PINS_H

#include "caos.h"

/* one row of the printed wiring */
typedef struct {
    const char *module;     /* pin name on the TFT module */
    const char *signal;     /* function */
    const char *mcu;        /* MCU pin */
    const char *nucleo;     /* connector position */
} lcd_wire_row_t;

#if MCU_ID == 575 || MCU_ID == 446
#define LCD_BOARD        "NUCLEO-U575ZI-Q / NUCLEO-F446ZE"
#define LCD_SPI_ALT      false
#define LCD_DC_PORT      CA_GPIO_PORT_F
#define LCD_DC_PIN       CA_GPIO_PIN_12
#define LCD_RST_PORT     CA_GPIO_PORT_F
#define LCD_RST_PIN      CA_GPIO_PIN_13
#define LCD_WIRE_SCK     { "SCK", "SPI1_SCK",  "PA5",  "CN7-10 (D13)" }
#define LCD_WIRE_SDA     { "SDA", "SPI1_MOSI", "PA7",  "CN7-14 (D11)" }
#define LCD_WIRE_DC      { "DC",  "GPIO",      "PF12", "CN7-20 (D8)" }
#define LCD_WIRE_RES     { "RES", "GPIO",      "PF13", "CN10-2 (D7)" }
#define LCD_WIRE_NOTE    "PA5/PA7: no Ethernet on this board, SPI1 default map"

#elif MCU_ID == 439 || MCU_ID == 767
#define LCD_BOARD        "NUCLEO-F439ZI / NUCLEO-F767ZI"
#define LCD_SPI_ALT      true
#define LCD_DC_PORT      CA_GPIO_PORT_F
#define LCD_DC_PIN       CA_GPIO_PIN_12
#define LCD_RST_PORT     CA_GPIO_PORT_F
#define LCD_RST_PIN      CA_GPIO_PIN_13
#define LCD_WIRE_SCK     { "SCK", "SPI1_SCK",  "PB3",  "CN7-15 (D23)" }
#define LCD_WIRE_SDA     { "SDA", "SPI1_MOSI", "PB5",  "CN7-13 (D22)" }
#define LCD_WIRE_DC      { "DC",  "GPIO",      "PF12", "CN7-20 (D8)" }
#define LCD_WIRE_RES     { "RES", "GPIO",      "PF13", "CN10-2 (D7)" }
#define LCD_WIRE_NOTE    "PA7 is the Ethernet RMII_CRS_DV (JP6): SPI1 alternate map PB3/PB5"

#elif MCU_ID == 753
#define LCD_BOARD        "NUCLEO-H753ZI"
#define LCD_SPI_ALT      true
#define LCD_DC_PORT      CA_GPIO_PORT_F
#define LCD_DC_PIN       CA_GPIO_PIN_3
#define LCD_RST_PORT     CA_GPIO_PORT_G
#define LCD_RST_PIN      CA_GPIO_PIN_12
#define LCD_WIRE_SCK     { "SCK", "SPI1_SCK",  "PB3",  "CN7-15 (D23) or CN12-31" }
#define LCD_WIRE_SDA     { "SDA", "SPI1_MOSI", "PB5",  "CN7-13 (D22)" }
#define LCD_WIRE_DC      { "DC",  "GPIO",      "PF3",  "CN7-20 (D8)" }
#define LCD_WIRE_RES     { "RES", "GPIO",      "PG12", "CN10-2 (D7)" }
#define LCD_WIRE_NOTE    "PA7 is the Ethernet (SB31): PB3/PB5; PB3 also goes to the ST-LINK SWO (SB32)"

#elif MCU_ID == 773
#define LCD_BOARD        "NUCLEO-H7S3L8"
#define LCD_SPI_ALT      true
#define LCD_DC_PORT      CA_GPIO_PORT_F
#define LCD_DC_PIN       CA_GPIO_PIN_5
#define LCD_RST_PORT     CA_GPIO_PORT_F
#define LCD_RST_PIN      CA_GPIO_PIN_4
#define LCD_WIRE_SCK     { "SCK", "SPI1_SCK",  "PB3",  "CN7-15 (D23)" }
#define LCD_WIRE_SDA     { "SDA", "SPI1_MOSI", "PB5",  "CN7-14 (D11)" }
#define LCD_WIRE_DC      { "DC",  "GPIO",      "PF5",  "CN7-20 (D8)" }
#define LCD_WIRE_RES     { "RES", "GPIO",      "PF4",  "CN10-2 (D7)" }
#define LCD_WIRE_NOTE    "SPI1 alternate map PB3/PB5; PB3 is shared with the ST-LINK SWO (SB45)"

#elif MCU_ID == 563
#define LCD_BOARD        "NUCLEO-H563ZI"
#define LCD_SPI_ALT      true
#define LCD_DC_PORT      CA_GPIO_PORT_F
#define LCD_DC_PIN       CA_GPIO_PIN_3
#define LCD_RST_PORT     CA_GPIO_PORT_G
#define LCD_RST_PIN      CA_GPIO_PIN_12
#define LCD_WIRE_SCK     { "SCK", "SPI1_SCK",  "PB3",  "CN12-31 (morpho)" }
#define LCD_WIRE_SDA     { "SDA", "SPI1_MOSI", "PB5",  "CN12-29 (morpho)" }
#define LCD_WIRE_DC      { "DC",  "GPIO",      "PF3",  "CN7-20 (D8)" }
#define LCD_WIRE_RES     { "RES", "GPIO",      "PG12", "CN10-2 (D7)" }
#define LCD_WIRE_NOTE    "PB3/PB5 do not reach CN7 on this board (SB33, SB17 OFF): morpho CN12"
#endif

#ifdef LCD_BOARD
#define LCD_PINS_OK      1

/* ST7735 on SPI1 at 10 MHz (APB2 80 MHz on U575; the prescaler is the same
 * on the others, the speed differs with their APB2), BL on TIM4_CH4 AF2 */
#define LCD_CFG(rot) {                                                         \
    .spi_num  = 1U, .br = CA_SPI_BR_DIV8, .alternate_gpio = LCD_SPI_ALT,      \
    .cs_port  = CA_GPIO_PORT_D, .cs_pin  = CA_GPIO_PIN_14,                    \
    .dc_port  = LCD_DC_PORT,    .dc_pin  = LCD_DC_PIN,                        \
    .rst_port = LCD_RST_PORT,   .rst_pin = LCD_RST_PIN,                       \
    .bl_tim   = 4U, .bl_ch = 4U, .bl_af = 2U,                                 \
    .bl_port  = CA_GPIO_PORT_D, .bl_pin  = CA_GPIO_PIN_15,                    \
    .col_off  = 0U, .row_off = 0U, .bgr = false, .invert = false,             \
    .rotation = (rot) }

static const lcd_wire_row_t s_lcd_wire[] __attribute__((unused)) = {
    LCD_WIRE_SCK,
    LCD_WIRE_SDA,
    { "CS",  "GPIO",            "PD14", "CN7-16 (D10)" },
    LCD_WIRE_DC,
    LCD_WIRE_RES,
    { "BL",  "TIM4_CH4 PWM AF2", "PD15", "CN7-18 (D9)" },
    { "GND", "-",               "-",    "CN7-8" },
    { "VCC", "3.3 V",           "-",    "CN8-7" },
};

#if ENABLE_CLI
/* The wiring of this board, one line per module pin. Immediate: runs in cli. */
static void __attribute__((unused)) lcd_wire_print(void)
{
    CLI_PRINTF_N("LCD WIRE %s (MCU %u)", LCD_BOARD, (u32)MCU_ID);
    CLI_PUT_N("LCD WIRE module  signal            MCU    Nucleo");
    for (u32 i = 0U; i < sizeof(s_lcd_wire) / sizeof(s_lcd_wire[0]); i++) {
        const lcd_wire_row_t *r = &s_lcd_wire[i];
        CLI_PRINTF_N("LCD WIRE %-6s  %-16s  %-5s  %s", r->module, r->signal, r->mcu, r->nucleo);
    }
    CLI_PRINTF_N("LCD WIRE note: %s", LCD_WIRE_NOTE);
    CLI_PUT_N("LCD WIRE END");
}
#endif

#else
#define LCD_PINS_OK      0
#endif

#endif /* LCD_PINS_H */
