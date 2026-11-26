// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    vnc_proto.h
 * @brief   RFB protocol (RFC 6143), server side: pure C, PC + MCU
 ******************************************************************************
 * No kernel, no TCP, no framebuffer driver: bytes in, bytes out through an
 * emit callback, pixels read from the framebuffer given at each update: L8
 * with its palette, or RGB565 (CA_VNC_PIX_*).
 * vnc_service.c puts it on a TCP connection; the fuzzer drives it directly.
 *
 * Handshake: the server offers 3.8. Minor < 7 is treated as 3.3, > 8 as 3.8
 * (RFC 6143 7.1.1); a major other than 3 or a malformed version line closes.
 * ONE security type is offered, p->sec, set before ca_vnc_proto_start():
 *   CA_VNC_SEC_NONE (1)       3.3 gets it as a u32; 3.7/3.8 the list [None];
 *                             3.8 also gets the SecurityResult
 *   CA_VNC_SEC_VENCRYPT (19)  3.7/3.8 only (a 3.3 client gets the failure
 *                             and the reason). VeNCrypt 0.2: the server
 *                             offers the single subtype X509None (260), the
 *                             client picks it, the server acks with 1 and
 *                             the state becomes CA_VNC_ST_TLS_START: the
 *                             caller runs the TLS handshake on the
 *                             connection, then ca_vnc_proto_tls_ready(), and
 *                             every byte from then on (SecurityResult on
 *                             3.8, ClientInit, ...) goes through TLS.
 *                             Bytes that follow the subtype in the same rx
 *                             call are TLS bytes: they are not consumed.
 *
 * Client messages are parsed byte by byte (TCP delivers any cut):
 *   SetPixelFormat     bpp 8/16/32, true colour only (colour map: close)
 *   SetEncodings       consumed and ignored: only Raw is ever sent
 *   FramebufferUpdateRequest   stored, answered by ca_vnc_proto_update()
 *   KeyEvent, PointerEvent     to the callbacks, if any (view only otherwise)
 *   ClientCutText      skipped by counting, never buffered
 *   anything else      close
 *
 * Updates: the framebuffer is cut in tiles of `tile` x `tile` pixels, each
 * with an FNV-1a hash of what the client was last sent. An incremental
 * request is answered only when some tile it touches changed, with the WHOLE
 * changed tiles (runs of adjacent tiles on a tile row merged into one rect):
 * the drawing side never has to say what it touched. A palette or pixel
 * format change resends everything. A non-incremental request gets its
 * rectangle (clipped to the framebuffer) at once. Pixels are converted to
 * the client's format one line at a time in `line` (width * 4 bytes).
 * Limit: a change that leaves a tile with the same 32-bit hash is missed.
 */
#ifndef VNC_PROTO_H
#define VNC_PROTO_H

#ifdef CAOS_PC_TEST
#include "caos_pc.h"
#else
#include "common.h"
#endif

#define CA_VNC_NAME_MAX     32U

/* Bytes out; false = the connection is gone, the caller closes */
typedef bool (*ca_vnc_emit_t)(void *ctx, const u8 *data, u32 len);

/* Optional converter (set after ca_vnc_proto_init): n rows of w source pixels
 * (src, src_stride bytes apart; fmt CA_VNC_PIX_RGB565, or CA_VNC_PIX_L8 through
 * `palette`, 256 entries 0x00RRGGBB) to the 32 bpp client pixel 0x00RRGGBB
 * little endian, packed in dst (w * 4 bytes per row). false = not done: the
 * protocol converts those rows itself. The service sets it where a DMA2D exists. */
typedef bool (*ca_vnc_conv_t)(u8 *dst, const u8 *src, u32 src_stride, u32 w, u32 n,
                              u8 fmt, const u32 *palette);
typedef void (*ca_vnc_key_cb_t)(bool down, u32 keysym);
typedef void (*ca_vnc_pointer_cb_t)(u8 buttons, u16 x, u16 y);

typedef struct {
    u8  bpp;                    /* 8, 16, 32 */
    u8  depth;
    u8  big_endian;
    u8  true_colour;
    u16 max[3];                 /* red, green, blue */
    u8  shift[3];
} ca_vnc_pixfmt_t;

/* Source pixels of ca_vnc_proto_update() */
#define CA_VNC_PIX_L8           0U  /* 1 byte: index in the palette */
#define CA_VNC_PIX_RGB565       1U  /* 2 bytes, little endian, 5-6-5 */

#define CA_VNC_SEC_NONE         1U
#define CA_VNC_SEC_VENCRYPT     19U
#define CA_VNC_VENCRYPT_X509NONE 260U
#define CA_VNC_VENCRYPT_X509PLAIN 262U  /* TLS, then user + password */
#define CA_VNC_USER_MAX         32U     /* X509Plain: longest user name */
#define CA_VNC_PASS_MAX         64U     /* X509Plain: longest password */

/* X509Plain: the credentials the client sent (inside TLS). true = accepted.
 * The buffers are wiped by the protocol right after the call. */
typedef bool (*ca_vnc_plain_cb_t)(const u8 *user, u32 ulen, const u8 *pass, u32 plen);

typedef enum {
    CA_VNC_ST_VERSION = 0,      /* waiting for the client's version line */
    CA_VNC_ST_SECURITY,         /* 3.7/3.8: waiting for the chosen type */
    CA_VNC_ST_VENC_VERSION,     /* VeNCrypt: waiting for the client's version */
    CA_VNC_ST_VENC_SUBTYPE,     /* VeNCrypt: waiting for the chosen subtype */
    CA_VNC_ST_TLS_START,        /* paused: the caller runs the TLS handshake */
    CA_VNC_ST_PLAIN_LEN,        /* X509Plain: waiting for the two lengths */
    CA_VNC_ST_PLAIN_DATA,       /* X509Plain: waiting for user + password */
    CA_VNC_ST_CLIENT_INIT,      /* waiting for the shared flag */
    CA_VNC_ST_MSG,              /* normal operation */
    CA_VNC_ST_CLOSED
} ca_vnc_state_t;

typedef struct {
    /* configuration (ca_vnc_proto_init) */
    u16                  width, height, tile;
    u16                  tiles_x, tiles_y;
    u32                 *tile_hash;         /* tiles_x * tiles_y words */
    u8                  *line;              /* width * 4 * line_rows bytes */
    u16                  line_rows;         /* rows `line` holds (1 by default) */
    ca_vnc_conv_t        conv;              /* RGB565/L8 -> 32 bpp LE, NULL = software */
    const u32           *pal;               /* L8: palette of the update in progress */
    /* Optional dirty map (the fb driver's, CA_FB_DIRTY_TILE grid): an
     * incremental update hashes only the tiles it marks (and those still to
     * send) instead of the whole frame, and clears what it consumed.
     * NULL = hash every tile of the request. */
    u32                 *dirty;
    u16                  dirty_tile, dirty_tx;  /* grid: tile pixels, tiles across */
    char                 name[CA_VNC_NAME_MAX + 1U];
    ca_vnc_emit_t        emit;
    void                *ctx;
    ca_vnc_key_cb_t      on_key;
    ca_vnc_pointer_cb_t  on_pointer;

    u8                   sec;               /* type offered: CA_VNC_SEC_* */
    u32                  venc_sub;          /* VeNCrypt subtype offered (X509NONE default) */
    ca_vnc_plain_cb_t    on_plain;          /* X509Plain check, NULL = every login refused */

    /* protocol state */
    ca_vnc_state_t       state;
    u8                   minor;             /* negotiated: 3, 7, 8 */
    u8                   acc[20];           /* current item */
    u8                   cred[CA_VNC_USER_MAX + CA_VNC_PASS_MAX]; /* X509Plain, wiped */
    u8                   ulen, clen;        /* X509Plain: user length, bytes received */
    u8                   alen;
    u8                   need;
    u32                  skip;              /* bytes still to drop */
    ca_vnc_pixfmt_t      pf;
    u32                  pal_hash;
    u8                   pix_fmt;           /* source format of the last update, 0xFF none */
    u32                  lut[256];          /* L8: palette in the client's format;
                                               RGB565: [0..31] red, [32..95] green,
                                               [96..127] blue contributions, OR-ed */

    /* pending FramebufferUpdateRequest */
    bool                 req;
    bool                 req_incr;
    u16                  rx, ry, rw, rh;

    /* statistics */
    u32                  updates, rects, pixels, keys, pointers;
    const char          *reason;            /* why it closed */
} ca_vnc_proto_t;

/**
 * @brief Prepares a session (one per client connection)
 * @param tile_hash storage for ceil(w/tile) * ceil(h/tile) words
 * @param line      storage for w * 4 bytes
 */
void ca_vnc_proto_init(ca_vnc_proto_t *p, u16 width, u16 height, u16 tile,
                       u32 *tile_hash, u8 *line, const char *name,
                       ca_vnc_emit_t emit, void *ctx);

/** @brief Sends the server version line. false = emit failed */
bool ca_vnc_proto_start(ca_vnc_proto_t *p);

/**
 * @brief Feeds client bytes. false = close the connection (p->reason)
 * @param used if not NULL, bytes consumed: less than len only when the
 *             state became CA_VNC_ST_TLS_START (the rest are TLS bytes)
 */
bool ca_vnc_proto_rx(ca_vnc_proto_t *p, const u8 *data, u32 len, u32 *used);

/** @brief TLS is up (CA_VNC_ST_TLS_START): go on inside it. false = emit failed */
bool ca_vnc_proto_tls_ready(ca_vnc_proto_t *p);

/**
 * @brief Answers the pending update request, if there is something to send
 * @param pix     first pixel of the framebuffer (width x height)
 * @param stride  bytes per line
 * @param fmt     CA_VNC_PIX_L8 or CA_VNC_PIX_RGB565; a change resends everything
 * @param palette 256 entries 0x00RRGGBB (L8; ignored for RGB565)
 * @return rectangles sent, 0 = nothing to send now (request still pending or
 *         none), -1 = emit failed
 */
i32 ca_vnc_proto_update(ca_vnc_proto_t *p, const u8 *pix, u32 stride, u8 fmt,
                        const u32 *palette);

/** @brief One pixel 0x00RRGGBB in the client's format (low bpp bits) */
u32 ca_vnc_proto_pixel(const ca_vnc_pixfmt_t *pf, u32 rgb);

#endif /* VNC_PROTO_H */
