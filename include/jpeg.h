// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    jpeg.h
 * @brief   JPEG codec: baseline 8-bit encode and decode, header included
 ******************************************************************************
 * The codec works on MCUs, not on raster images: 8x8 blocks of samples, in
 * the order of the minimum coded unit of the chroma subsampling:
 *
 *   CA_JPEG_GRAY  8 x 8   Y                     64 bytes per MCU
 *   CA_JPEG_444   8 x 8   Y Cb Cr              192
 *   CA_JPEG_422  16 x 8   Y0 Y1 Cb Cr          256
 *   CA_JPEG_420  16 x 16  Y0 Y1 Y2 Y3 Cb Cr    384   (Y blocks left-right, top-bottom)
 *
 * Every block is 64 bytes, row by row. ca_jpeg_rgb_to_mcu() and
 * ca_jpeg_mcu_to_rgb() convert from and to RGB888 (R, G, B bytes) or 8-bit
 * gray, with the JFIF colour equations in fixed point, chroma averaged over
 * its area when encoding and repeated when decoding; edge MCUs of a size not
 * multiple of the MCU repeat the last row and column.
 *
 * Encoding writes the whole file: SOI, DQT, SOF0, DHT, SOS, data, EOI, with
 * the Annex K quantization tables scaled for `quality` (1..100, the libjpeg
 * scale) and the Annex K Huffman tables. Decoding parses the header in
 * hardware (baseline only: gray or 3-component YCbCr with Y sampling 1x1,
 * 2x1 or 2x2) and returns its geometry in ca_jpeg_info_t.
 *
 * Data goes through the 32-byte FIFOs polled by the CPU; a truncated or
 * corrupt stream ends on the timeout, never blocks.
 *
 * Present on F767 and H7; elsewhere ca_jpeg_init() returns false.
 * One user at a time, task context only.
 */
#ifndef JPEG_H
#define JPEG_H

#include "common.h"
#include "os_config.h"

#if EN_JPEG

typedef enum {
    CA_JPEG_GRAY = 0,
    CA_JPEG_444,
    CA_JPEG_422,
    CA_JPEG_420
} ca_jpeg_css_t;

typedef struct {
    u16           width;
    u16           height;
    ca_jpeg_css_t css;
    u8            quality;      /* 1..100, encoding only */
} ca_jpeg_cfg_t;

typedef struct {
    u16           width;
    u16           height;
    ca_jpeg_css_t css;
    u32           mcus;         /* MCUs of the image */
    u32           mcu_bytes;    /* total MCU data, mcus x bytes per MCU */
} ca_jpeg_info_t;

/* Error flags of the last operation (ca_jpeg_errors) */
#define CA_JPEG_ERR_ARG     0x01U   /* size, subsampling, quality or buffer refused */
#define CA_JPEG_ERR_TMO     0x02U   /* not finished within the timeout (truncated stream?) */
#define CA_JPEG_ERR_OVF     0x04U   /* the output buffer was too small */
#define CA_JPEG_ERR_HDR     0x08U   /* header not supported (CMYK, other sampling) */
/* Status bits of ca_jpeg_hw_status() */
#define CA_JPEG_HW_IN_FREE  0x100U  /* input FIFO not full */
#define CA_JPEG_HW_OUT_DATA 0x200U  /* output FIFO not empty */
#define CA_JPEG_HW_EOC      0x400U  /* end of conversion */
#define CA_JPEG_HW_HDR      0x800U  /* header parsed */

bool ca_jpeg_init(void);            /* false = no JPEG codec */
bool ca_jpeg_geometry(u16 width, u16 height, ca_jpeg_css_t css, ca_jpeg_info_t *info);
bool ca_jpeg_encode(const u8 *mcu, const ca_jpeg_cfg_t *cfg,
                    u8 *out, u32 out_max, u32 *out_len, u32 timeout_ms);
bool ca_jpeg_decode(const u8 *jpg, u32 len, u8 *mcu, u32 mcu_max,
                    ca_jpeg_info_t *info, u32 timeout_ms);
u32  ca_jpeg_errors(void);          /* CA_JPEG_ERR_* */

/* Software conversions (gray: 1 byte per pixel in and out) */
void ca_jpeg_rgb_to_mcu(const u8 *img, u16 width, u16 height, ca_jpeg_css_t css, u8 *mcu);
void ca_jpeg_mcu_to_rgb(const u8 *mcu, u16 width, u16 height, ca_jpeg_css_t css, u8 *img);

#endif /* EN_JPEG */

#endif /* JPEG_H */
