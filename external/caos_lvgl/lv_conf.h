/*
 * caOS LVGL configuration - the ONLY one, for liblvgl.a and for every file
 * that includes lvgl.h (LV_CONF_INCLUDE_SIMPLE + this directory on the
 * include path of both): one sizeof for every LVGL struct, no ODR mismatch
 * as the one FatFS had ([[project_fatfs_ffconf_odr_bug]]).
 *
 * It must NOT include os_config.h: the library would be rebuilt for every
 * project. What depends on the project (extra heap, task, display size)
 * lives in the caOS port layer, not here.
 *
 * Only what differs from LVGL's defaults (src/lv_conf_internal.h fills in
 * the rest). LVGL v9.6.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- colour ----------------------------------------------------------- */
#define LV_COLOR_FORMAT_DEFAULT         LV_COLOR_FORMAT_RGB565

/* ---- memory ------------------------------------------------------------
 * First pool from the caOS heap at lv_init(), owned by the task that calls
 * it (the GUI task): it goes back with that task, as any caOS block. The
 * size is fixed here because TLSF sizes its index from it; more room is
 * added at run time with lv_mem_add_pool() (pools up to this size each). */
#define LV_USE_STDLIB_MALLOC            LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING            LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF           LV_STDLIB_BUILTIN
#define LV_MEM_SIZE                     (32U * 1024U)
#define LV_MEM_POOL_INCLUDE             "caos_lvgl_port.h"
#define LV_MEM_POOL_ALLOC               caos_lvgl_pool_alloc

/* ---- OS ----------------------------------------------------------------
 * One GUI task owns LVGL (it is not thread-safe); other tasks hand work to
 * it with lv_async_call(). */
#define LV_USE_OS                       LV_OS_NONE
#define LV_DEF_REFR_PERIOD              33          /* ms, ~30 fps */
#define LV_DPI_DEF                      130

/* ---- software renderer: only the formats caOS draws in ----------------
 * Each format enabled here links its blend routines whether used or not. */
#define LV_DRAW_SW_SUPPORT_RGB565               1
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED       0
#define LV_DRAW_SW_SUPPORT_RGB565A8             0
#define LV_DRAW_SW_SUPPORT_RGB888               0
#define LV_DRAW_SW_SUPPORT_XRGB8888             0
#define LV_DRAW_SW_SUPPORT_ARGB8888             1   /* images with alpha */
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
#define LV_DRAW_SW_SUPPORT_L8                   0
#define LV_DRAW_SW_SUPPORT_AL88                 0
#define LV_DRAW_SW_SUPPORT_A8                   1   /* alpha masks, icons */
#define LV_DRAW_SW_SUPPORT_I1                   0

/* LVGL's own DMA2D backend stays off: caOS has its drawing unit on
 * ca_dma2d (the IRQ belongs to dma2d.c, D-cache off on H7/F7). */
#define LV_USE_DRAW_DMA2D               0

/* ---- features ---------------------------------------------------------- */
#define LV_USE_LOG                      0
#define LV_USE_ASSERT_NULL              1
#define LV_USE_ASSERT_MALLOC            1
#define LV_USE_FLOAT                    0
#define LV_USE_SYSMON                   0
#define LV_USE_PERF_MONITOR             0
#define LV_USE_SNAPSHOT                 0
#define LV_BUILD_EXAMPLES               0
#define LV_BUILD_DEMOS                  0

/* ---- fonts: unused ones cost nothing (gc-sections) -------------------- */
#define LV_FONT_MONTSERRAT_12           1
#define LV_FONT_MONTSERRAT_14           1
#define LV_FONT_MONTSERRAT_16           1
#define LV_FONT_MONTSERRAT_20           1
#define LV_FONT_UNSCII_8                1
#define LV_FONT_DEFAULT                 &lv_font_montserrat_14

#endif /* LV_CONF_H */
