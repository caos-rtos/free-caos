/**
 ******************************************************************************
 * @file    gfx_lvgl.h
 * @brief   LVGL in caOS: drawing unit on ca_dma2d
 ******************************************************************************
 * LVGL (external/lvgl, configuration external/caos_lvgl/lv_conf.h) draws in
 * software. ca_lvgl_dma2d_init() adds a drawing unit that takes from it what
 * the DMA2D does in one pass:
 *
 *   fill      no radius, no gradient            -> ca_dma2d_fill / _blend (opacity)
 *   image     plain bitmap in memory, no scale, rotation, skew, mask, recolor
 *             or tiling; RGB565 ARGB8888 XRGB8888 RGB888 ARGB1555
 *                                               -> ca_dma2d_copy / _convert / _blend
 *
 * on RGB565 and ARGB8888 layers. Everything else stays with the software
 * renderer. Synchronous: the GUI task sleeps on the DMA2D interrupt while a
 * transfer runs. An operation ca_dma2d refuses (buffer out of its reach,
 * alignment) is drawn in software instead and counted in `fallback_sw`.
 *
 * LVGL is not thread-safe: init, enable and the drawing all belong to the
 * GUI task. The DMA2D has one user at a time: while LVGL uses it, nobody
 * else should.
 ******************************************************************************
 */
#ifndef GFX_LVGL_H
#define GFX_LVGL_H

#include "common.h"
#include "os_config.h"

#if EN_FB
#include "fb.h"

/* ---- LVGL on a caOS framebuffer -----------------------------------------
 * ca_lvgl_start() creates the GUI task, which owns LVGL from then on: it
 * calls lv_init(), makes a display of the framebuffer's size drawing into a
 * partial buffer of `buf_lines` lines, flushes each finished area into the
 * framebuffer with ca_fb_blit(), adds a pointer and a keypad input device
 * (default group: widgets created in `setup` take the keys), adds the DMA2D
 * unit where there is one, calls `setup` to build the screens, then runs
 * lv_timer_handler() forever.
 *
 * The framebuffer is RGB565 and belongs to whoever shows it (VNC, LTDC): the
 * GUI task only writes into it through the handle.
 *
 * Input from other tasks (VNC callbacks, a local driver):
 * ca_lvgl_input_pointer() and ca_lvgl_input_key() only queue; the GUI task
 * reads them. Any other work on LVGL objects from another task goes through
 * ca_lvgl_call(): fn runs in the GUI task, at its next loop. */

typedef void (*ca_lvgl_fn_t)(void *arg);

typedef struct {
    ca_fb_t      fb;            /* RGB565, its size is the screen size */
    u16          buf_lines;     /* partial draw buffer, lines (0 = 20) */
    u32          extra_heap;    /* bytes added to LVGL's 32 KB pool (0 = none) */
    bool         use_dma2d;     /* add the DMA2D unit where the part has one */
    bool         cursor;        /* draw a pointer arrow in the framebuffer: the
                                   only cursor a viewer shows (VNC sends none),
                                   and the one a panel with a mouse needs */
    ca_lvgl_fn_t setup;         /* builds the UI, in the GUI task, after init */
    void        *setup_arg;
} ca_lvgl_cfg_t;

/* false: fb not RGB565, LVGL already running, or the GUI task could not be
 * created. The init itself runs in the GUI task: its failures stop that task
 * with a message (a later start is allowed). */
bool ca_lvgl_start(const ca_lvgl_cfg_t *cfg);

/* Ends LVGL: the GUI task leaves its loop, calls lv_deinit() and ends; caOS
 * gives back its heap (LVGL's pool, the draw buffer, extra_heap) - every
 * LVGL object is gone. The framebuffer is not touched: it keeps the last
 * frame and stays its owner's. Waits up to 2 s for the task. true: ended (or
 * was not running); false: still running, or called from the GUI task.
 * ca_lvgl_start() can run again afterwards. */
bool ca_lvgl_stop(void);

/* Queue input for LVGL, from any task (not from an ISR). Pointer: the last
 * state wins, bit 0 of buttons = pressed. Key: an X11 keysym as VNC gives it
 * (ASCII as is; BackSpace, Return, Tab, Escape, Delete, Home, End, arrows
 * mapped to LV_KEY_*; modifiers dropped). false = key queue full. */
void ca_lvgl_input_pointer(u8 buttons, u16 x, u16 y);
bool ca_lvgl_input_key(bool down, u32 keysym);

/* fn(arg) in the GUI task, which the call wakes. false = queue full */
bool ca_lvgl_call(ca_lvgl_fn_t fn, void *arg);

/* fn(arg) in the GUI task, the caller waiting for it to have run (from the
 * GUI task itself: run at once). One caller at a time, the others wait their
 * turn. false: LVGL not running, queue full, or not run within timeout_ms -
 * then fn may still run later: arg must stay valid, and a GUI task that
 * stops answering should not be called again. */
bool ca_lvgl_call_sync(ca_lvgl_fn_t fn, void *arg, u32 timeout_ms);

#endif /* EN_FB */

#if EN_DMA2D

typedef struct {
    u32 fills;          /* opaque fills (ca_dma2d_fill) */
    u32 fill_blends;    /* fills with opacity (ca_dma2d_blend, A8 foreground) */
    u32 copies;         /* opaque images, same format (ca_dma2d_copy) */
    u32 converts;       /* opaque images, other format (ca_dma2d_convert) */
    u32 img_blends;     /* images with alpha or opacity (ca_dma2d_blend) */
    u32 fallback_sw;    /* refused by ca_dma2d, drawn in software */
    u32 last_error;     /* ca_dma2d_errors() of the last refusal */
    u32 glyphs;         /* glyphs blended by the DMA2D */
    u32 glyphs_sw;      /* glyphs left to the CPU (under the threshold, or not A1..A8) */
} ca_lvgl_dma2d_stats_t;

/* After lv_init(), from the GUI task. false: no DMA2D on this part (LVGL
 * keeps drawing in software, nothing else changes). */
bool ca_lvgl_dma2d_init(void);

/* false: the unit claims nothing, every task goes to the software renderer.
 * For A/B comparisons; on by default after init. */
void ca_lvgl_dma2d_enable(bool on);

void ca_lvgl_dma2d_stats(ca_lvgl_dma2d_stats_t *st, bool reset);

/* Text on the DMA2D: each glyph (A8 after LVGL unpacks it) is blended with
 * the text colour, the GUI task polling the transfer. Glyphs whose visible
 * area is below min_px pixels stay on the CPU. Default 0 = every glyph
 * (measured best on F439 F767 H753 U575); CA_LVGL_TEXT_OFF = text all in
 * software. */
#define CA_LVGL_TEXT_OFF       0xFFFFFFFFU
void ca_lvgl_dma2d_text(u32 min_px);

#endif /* EN_DMA2D */
#endif /* GFX_LVGL_H */
