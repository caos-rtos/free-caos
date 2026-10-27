// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    dma2d.h
 * @brief   Chrom-ART accelerator (DMA2D): fill, copy, convert, blend rectangles
 ******************************************************************************
 * A DMA for images: every operation works on a w x h rectangle inside larger
 * images, each with its own line length (stride, in pixels). Four kinds:
 *
 *   fill      a fixed color into the destination           (register-to-memory)
 *   copy      source to destination, same format           (memory-to-memory)
 *   convert   source to destination in another format      (with PFC)
 *   blend     foreground over background into destination  (PFC + blending)
 *
 * No logic operations (AND/OR/XOR): mixing is alpha blending only,
 *   aM = aF aB / 255,  aO = aF + aB - aM,  C = (CF aF + CB aB - CB aM) / aO
 * with every division rounded down (RM0410 9.3.6).
 *
 * Inputs: the 11 formats of ca_dma2d_fmt_t. L8/L4/AL44/AL88 index a CLUT
 * of up to 256 ARGB8888 entries, loaded by the DMA2D itself or by the CPU;
 * A8/A4 carry only alpha, the color is the source's `color`. Outputs: the
 * first five formats. Direct-color channels are widened by copying their
 * top bits into the bottom ones.
 *
 * Alignment (else the driver refuses): the address on the pixel size (32/16
 * bit, bytes for 24/8/4 bit); for 4-bit formats an even width and stride.
 * Width up to 16383, stride - width up to 16383, height up to 65535.
 *
 * timeout_ms = CA_NO_WAIT_TIME starts the operation and returns: then
 * ca_dma2d_wait() or ca_dma2d_abort(). Buffers anywhere the DMA2D reaches:
 * on H7 not the DTCM (the task stacks), so static. D-cache is off on F7/H7.
 *
 * Present on F767 (F746), H7 and U5; elsewhere ca_dma2d_init() returns false.
 * One user at a time, task context only.
 */
#ifndef DMA2D_H
#define DMA2D_H

#include "common.h"
#include "os_config.h"

#if EN_DMA2D

/* Pixel formats; the value is the hardware CM code */
typedef enum {
    CA_DMA2D_ARGB8888 = 0,  /* input and output */
    CA_DMA2D_RGB888,        /* input and output, 3 bytes */
    CA_DMA2D_RGB565,        /* input and output */
    CA_DMA2D_ARGB1555,      /* input and output */
    CA_DMA2D_ARGB4444,      /* input and output */
    CA_DMA2D_L8,            /* input: CLUT index */
    CA_DMA2D_AL44,          /* input: alpha 4 + index 4 */
    CA_DMA2D_AL88,          /* input: alpha 8 + index 8 */
    CA_DMA2D_L4,            /* input: CLUT index, two per byte */
    CA_DMA2D_A8,            /* input: alpha only, color fixed */
    CA_DMA2D_A4             /* input: alpha only, two per byte */
} ca_dma2d_fmt_t;

#define CA_DMA2D_OUT_FMTS      5U       /* formats 0..4 can be written */

/* Alpha of a source after its format conversion */
typedef enum {
    CA_DMA2D_ALPHA_KEEP = 0,    /* the pixel's own (0xFF without an alpha channel) */
    CA_DMA2D_ALPHA_REPLACE,     /* `alpha` */
    CA_DMA2D_ALPHA_MULT         /* pixel alpha * `alpha` / 255 */
} ca_dma2d_am_t;

/* flags of a source or destination */
#define CA_DMA2D_AI            0x01U    /* alpha inverted (0 = opaque) */
#define CA_DMA2D_RBS           0x02U    /* red and blue swapped (BGR) */
#define CA_DMA2D_CLUT_CPU      0x04U    /* source: the CPU writes the CLUT, not the DMA2D */

typedef struct {
    const void     *addr;       /* first pixel of the rectangle */
    u32             stride;     /* pixels per line of the whole image */
    ca_dma2d_fmt_t  fmt;
    ca_dma2d_am_t   am;
    u8              alpha;      /* for REPLACE and MULT */
    u8              flags;      /* CA_DMA2D_AI | _RBS | _CLUT_CPU */
    u32             color;      /* 0xRRGGBB for A8/A4 */
    const u32      *clut;       /* ARGB8888 entries for L8/L4/AL44/AL88, NULL = keep the loaded one */
    u16             clut_n;     /* entries, 1..256 */
} ca_dma2d_src_t;

typedef struct {
    void           *addr;       /* first pixel of the rectangle */
    u32             stride;     /* pixels per line of the whole image */
    ca_dma2d_fmt_t  fmt;        /* CA_DMA2D_ARGB8888..ARGB4444 */
    u8              flags;      /* CA_DMA2D_AI | _RBS */
} ca_dma2d_dst_t;

/* Error flags of the last operation (ca_dma2d_errors) */
#define CA_DMA2D_ERR_TE        0x01U    /* bus error on an access */
#define CA_DMA2D_ERR_CE        0x02U    /* configuration refused by the hardware */
#define CA_DMA2D_ERR_CAE       0x04U    /* CLUT accessed while in use */
#define CA_DMA2D_ERR_TMO       0x08U    /* no completion within the timeout */
#define CA_DMA2D_ERR_ARG       0x10U    /* refused by the driver: size, format, alignment */
/* Status of ca_dma2d_hw_irq() besides the errors */
#define CA_DMA2D_HW_TC         0x100U   /* transfer complete */

bool ca_dma2d_init(void);           /* false = no DMA2D */
bool ca_dma2d_fill(const ca_dma2d_dst_t *d, u32 w, u32 h, u32 color, u32 timeout_ms);  /* color in d->fmt */
bool ca_dma2d_copy(const ca_dma2d_src_t *s, const ca_dma2d_dst_t *d, u32 w, u32 h, u32 timeout_ms);
bool ca_dma2d_convert(const ca_dma2d_src_t *s, const ca_dma2d_dst_t *d, u32 w, u32 h, u32 timeout_ms);
bool ca_dma2d_blend(const ca_dma2d_src_t *fg, const ca_dma2d_src_t *bg,
                    const ca_dma2d_dst_t *d, u32 w, u32 h, u32 timeout_ms);
bool ca_dma2d_wait(u32 timeout_ms);  /* the operation started with CA_NO_WAIT_TIME */
void ca_dma2d_abort(void);
bool ca_dma2d_busy(void);
u32  ca_dma2d_errors(void);          /* CA_DMA2D_ERR_* */
u32  ca_dma2d_bpp(ca_dma2d_fmt_t fmt);   /* bits per pixel, 0 = unknown */

#endif /* EN_DMA2D */

#endif /* DMA2D_H */
