// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    fb.h
 * @brief   Framebuffer: a canvas in RAM, shown by whoever owns it
 ******************************************************************************
 * A framebuffer is a w x h image in heap RAM (no MCU has dedicated VRAM).
 * It knows nothing about who shows it: the VNC service, the LTDC, a test.
 *
 * Size is free, never dictated by a panel: a display shows a WINDOW of it.
 * The LTDC does that with no copy: start address = base + y*stride + x,
 * pitch (CFBP) = stride, line length (CFBLL) = window width (RM0433 31.7.23).
 * The window belongs to the display driver, not to the framebuffer.
 *
 * Formats:
 *   L8      one byte per pixel = index in a palette of 256 colors (0x00RRGGBB).
 *           The LTDC and the DMA2D read L8 through their CLUT (the DMA2D cannot
 *           write it). Black and white = two entries. Default palette: gray
 *           ramp, entry i = (i, i, i). The format for big canvases.
 *   RGB565  two bytes per pixel, little endian, 5-6-5: what LVGL draws and the
 *           DMA2D writes. No palette. 320 x 240 = 150 KB; 640 x 480 (600 KB)
 *           fits no MCU of the project.
 * A colour argument is a value in the framebuffer's format: an index for L8,
 * a 16-bit RGB565 word for RGB565.
 *
 * Handle: a ca_fb_t is a slot index plus a generation, never a pointer. A
 * stale handle (destroyed framebuffer, dead owner) is refused by every call.
 *
 * Ownership. The pixels are one shared heap block: it belongs to no task and
 * survives its creator. The service that shows it (VNC, LTDC) takes it with
 * ca_fb_take() from ITS task: from then on the block dies with that task,
 * and only that task can destroy it. Drawing goes through the handle only:
 * every primitive checks, with the scheduler locked, that the owner is still
 * alive before touching a line, so nobody writes into freed memory.
 * ca_fb_get() is for the reader (VNC, LTDC): read-only, and valid while the
 * reader is the owner.
 * Limit: an owner restarted by a fault (CA_FAULT_RESTART_THREAD) keeps its
 * task handle but loses its blocks; it must not keep the old framebuffer.
 *
 * Drawing clips silently: what falls outside is skipped, the return value is
 * the number of pixels written (0 all outside), -1 for a stale handle.
 *
 * Double buffer (buffers = 2, vnc_add.md D10), for "gaming" screens; one
 * buffer is the default and costs nothing extra. Two are asked: if they do
 * not fit, one is allocated (info.buffers says which), none = failure. The
 * application always draws in A (the same address: LVGL's partial rendering
 * needs the previous content); at the end of a frame - ca_fb_present(), or
 * nothing drawn for 100 ms - A is copied to B, at most every FB_COPY_MS
 * (os_config, default 50), and the reader (VNC) reads B: never half a frame.
 * The copy is one DMA2D command for the whole screen where the part has one
 * (0.5 ms for 300 KB on H753), else the CPU copies the changed tiles only.
 * B is fb's own: ca_fb_get() and ca_fb_get_pixel() always give A, where the
 * application draws; only the reader sees B, through ca_fb_info().base
 * (B with two buffers, A with one).
 * Task context only, never from ISR.
 */
#ifndef FB_H
#define FB_H

#include "common.h"
#include "os_config.h"

#if EN_FB

#include "scheduler.h"

#define CA_FB_MAX           2U          /* framebuffers alive at the same time */
#define CA_FB_PALETTE_SIZE  256U
#define CA_FB_NONE          0U          /* never a valid handle */

/* Dirty map: every drawing call (set_pixel, fill_rect, clear, blit) marks the
 * CA_FB_DIRTY_TILE x CA_FB_DIRTY_TILE tiles it touches; a new framebuffer and
 * set_palette mark them all. One reader (the VNC service) takes the map and
 * sends only what changed, instead of re-reading the whole frame. Bit of tile
 * (tx, ty): index ty * tiles_x + tx, bit (index & 31) of word (index >> 5). */
#define CA_FB_DIRTY_TILE    16U
#define CA_FB_DIRTY_TX(w)   (((u32)(w) + CA_FB_DIRTY_TILE - 1U) / CA_FB_DIRTY_TILE)
#define CA_FB_DIRTY_TY(h)   (((u32)(h) + CA_FB_DIRTY_TILE - 1U) / CA_FB_DIRTY_TILE)
#define CA_FB_DIRTY_WORDS(w, h) ((CA_FB_DIRTY_TX(w) * CA_FB_DIRTY_TY(h) + 31U) / 32U)

typedef u32 ca_fb_t;

typedef enum {
    CA_FB_FMT_L8 = 0,           /* 1 byte per pixel, palette index */
    CA_FB_FMT_RGB565            /* 2 bytes per pixel, little endian, no palette */
} ca_fb_fmt_t;

typedef struct {
    const u8    *base;          /* first pixel of the buffer being shown */
    u16          width;         /* pixels */
    u16          height;        /* lines */
    u32          stride;        /* bytes from one line to the next */
    u32          size;          /* bytes of one buffer */
    ca_fb_fmt_t  fmt;
    u8           bpp;           /* bits per pixel */
    u8           buffers;       /* buffers obtained: 1 or 2 */
    const u32   *palette;       /* CA_FB_PALETTE_SIZE entries, 0x00RRGGBB (L8 only) */
    u32          palette_gen;   /* +1 at every palette change: reload the CLUT */
    ca_task_handle_t owner;     /* CA_TASK_HANDLE_NONE = shared, nobody took it */
    u32          copies;        /* double buffer: copies A -> B done */
} ca_fb_info_t;

/**
 * @brief Creates a framebuffer (shared, no owner), cleared to 0
 * @param width   pixels, > 0
 * @param height  lines, > 0
 * @param fmt     CA_FB_FMT_L8 or CA_FB_FMT_RGB565
 * @param buffers 1, or 2 (double buffer, falls back to 1 if it does not fit)
 * @return the handle, CA_FB_NONE on bad arguments, no free slot or no memory
 */
ca_fb_t ca_fb_create(u16 width, u16 height, ca_fb_fmt_t fmt, u8 buffers);

/**
 * @brief The calling task takes the framebuffer (the service that shows it)
 * @return true if taken (or already its own); false if stale, called outside
 *         a task, or another task owns it
 */
bool ca_fb_take(ca_fb_t fb);

/**
 * @brief Frees the framebuffer; the handle becomes stale
 * @details Only the owner, or anyone while nobody took it.
 * @return true if freed
 */
bool ca_fb_destroy(ca_fb_t fb);

/**
 * @brief The pixels the application draws (buffer A), read-only
 * @details A reader (VNC) uses ca_fb_info().base: with a double buffer that
 *          is B, the last complete frame.
 * @return first pixel, NULL if stale
 */
const u8 *ca_fb_get(ca_fb_t fb);

/**
 * @brief Geometry, format, palette and owner
 * @return false if stale
 */
bool ca_fb_info(ca_fb_t fb, ca_fb_info_t *info);

/**
 * @brief Writes palette entries [first, first + count)
 * @param rgb count entries, 0x00RRGGBB
 * @return false if stale or the range exceeds 256 (RGB565: accepted, unused)
 */
bool ca_fb_set_palette(ca_fb_t fb, u16 first, u16 count, const u32 *rgb);

/* ---- drawing: clipped, return pixels written, -1 stale handle ---------- */

i32 ca_fb_set_pixel(ca_fb_t fb, i32 x, i32 y, u32 color);
i32 ca_fb_fill_rect(ca_fb_t fb, i32 x, i32 y, i32 w, i32 h, u32 color);
i32 ca_fb_clear(ca_fb_t fb, u32 color);

/**
 * @brief Copies a w x h block of pixels, already in the framebuffer's format,
 *        to (x, y). What falls outside is skipped (the source is clipped too).
 * @param src        first pixel of the block
 * @param src_stride bytes from one source line to the next
 * @details The way a renderer (LVGL's flush) puts its pixels in: one line at
 *          a time under the same owner check as the other primitives.
 */
i32 ca_fb_blit(ca_fb_t fb, i32 x, i32 y, i32 w, i32 h, const void *src, u32 src_stride);

/**
 * @brief The frame drawn so far is complete (double buffer: it may be copied
 *        to the shown buffer now, or as soon as FB_COPY_MS allow). No effect
 *        with one buffer. LVGL's port calls it on the last flush of a frame.
 * @return false if stale
 */
bool ca_fb_present(ca_fb_t fb);

/**
 * @brief Takes the dirty map: ORs the framebuffer's into map and clears it
 * @details Double buffer: first copies a complete frame to B if one is due,
 *          then hands the changes of B.
 * @param map   CA_FB_DIRTY_WORDS(width, height) words (or more)
 * @return false if stale or words too few (map untouched)
 */
bool ca_fb_take_dirty(ca_fb_t fb, u32 *map, u32 words);

/**
 * @brief Marks a rectangle changed, for pixels written NOT through the calls
 *        above (a DMA, a cast of info.base): else the VNC does not see them
 * @return pixels covered after clipping, -1 if stale
 */
i32 ca_fb_mark_dirty(ca_fb_t fb, i32 x, i32 y, i32 w, i32 h);

/**
 * @brief Reads one pixel
 * @return the value in the framebuffer's format (index, or RGB565), -1 if
 *         stale or outside
 */
i32 ca_fb_get_pixel(ca_fb_t fb, i32 x, i32 y);

#endif /* EN_FB */
#endif /* FB_H */
