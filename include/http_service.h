// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

#pragma once
#include "def_mcu.h"
#include "os_config.h"

#if EN_HTTP_SERVICE && EN_NET && EN_TCP_SERVICE

/* ── Methods ──────────────────────────────────────────────────────────────── */
#define HTTP_METHOD_GET     0x01U
#define HTTP_METHOD_POST    0x02U
#define HTTP_METHOD_HEAD    0x04U
#define HTTP_METHOD_OPTIONS 0x08U

/* ── Buffer sizes ─────────────────────────────────────────────────────────── */
#ifndef HTTP_REQ_BUF_SIZE
#define HTTP_REQ_BUF_SIZE   2048U   /* browser headers >> curl: User-Agent, Accept, etc. */
#endif
#ifndef HTTP_RESP_BUF_SIZE
#define HTTP_RESP_BUF_SIZE  4096U   /* covers long CLI output (top, help, etc.) */
#endif
#ifndef HTTP_URI_MAX
#define HTTP_URI_MAX        128U
#endif
#ifndef HTTP_QUERY_MAX
#define HTTP_QUERY_MAX      128U
#endif
#ifndef HTTP_DEFAULT_PORT
#define HTTP_DEFAULT_PORT   80U
#endif
#ifndef HTTP_RECV_TIMEOUT_MS
#define HTTP_RECV_TIMEOUT_MS  1500U
#endif

/* ── Request ──────────────────────────────────────────────────────────────── */
typedef struct {
    u8   method;                    /* HTTP_METHOD_* */
    char uri[HTTP_URI_MAX];         /* path, e.g. "/status" */
    char query[HTTP_QUERY_MAX];     /* query string after '?', e.g. "state=on" */
    const u8 *body;                 /* pointer in the rx buffer (POST body) */
    u16  body_len;
} ca_http_request_t;

/* ── Transport ────────────────────────────────────────────────────────────────
 * Where a connection's bytes go: plain TCP for http_service, a TLS session for
 * https_service. The parser, the routes and the response builder are the same.
 *   send: true when all len bytes were written
 *   recv: > 0 bytes read, 0 on timeout, < 0 when the peer is gone */
typedef struct {
    bool  (*send)(void *io, const u8 *data, u16 len);
    i32   (*recv)(void *io, u8 *buf, u16 len, u32 timeout_ms);
    void   *io;
} ca_http_io_t;

/* ── Response builder ─────────────────────────────────────────────────────── */
typedef struct {
    u8  *buf;           /* output buffer (provided by the service) */
    u16  size;          /* buffer size */
    u16  len;           /* bytes written so far */
    u16  cl_pos;        /* offset of the Content-Length placeholder in the buf (0 = not used) */
    u16  hdr_end;       /* offset where the body starts (after \r\n\r\n) */
    bool head_only;     /* true if HEAD method: skip body */
    /* ── chunked transfer (RFC 7230 §4.1) ────────────────────────────────
     * Set by ca_http_resp_begin_chunked(). In this mode the body is NOT
     * accumulated: every flush leaves the buffer as a self-contained chunk on
     * the wire, so the response is not bounded by `size`. Content-Length is
     * absent by definition -- which is the whole point: it cannot be patched
     * afterwards if the body was never held in memory. */
    bool chunked;       /* true = emit chunks instead of one buffered body */
    const ca_http_io_t *io;     /* transport used to flush chunks */
    bool failed;        /* a flush failed: stop writing, close the connection */
} ca_http_response_t;

/* Writes HTTP headers + Content-Type. Call before the body. */
void ca_http_resp_begin(ca_http_response_t *r, u16 status,
                        const char *content_type);

/* Adds text to the body (no-op if head_only). */
void ca_http_resp_write(ca_http_response_t *r, const char *s, u16 len);

/* Adds C string to the body. */
void ca_http_resp_puts(ca_http_response_t *r, const char *s);

/* Adds decimal integer to the body. */
void ca_http_resp_puti(ca_http_response_t *r, u32 v);

/* Pre-built 404 response. */
void ca_http_resp_not_found(ca_http_response_t *r);

/* Pre-built 400 response. */
void ca_http_resp_bad_request(ca_http_response_t *r);

/* ── Chunked responses — for bodies larger than HTTP_RESP_BUF_SIZE ────────
 *
 * Use instead of ca_http_resp_begin() when the body has no known bound: CLI
 * output, file dumps, long listings. The buffer is then only a staging area,
 * emptied onto the wire whenever it fills, so the response size is unlimited
 * and independent of HTTP_RESP_BUF_SIZE.
 *
 * The regular ca_http_resp_write/puts/puti keep working unchanged -- they
 * detect the mode and flush as needed. The service emits the terminating
 * 0-length chunk on its own once the handler returns: do not write it.
 *
 *   ca_http_resp_begin_chunked(resp, 200U, "text/plain");
 *   for (...) ca_http_resp_puts(resp, line);     // any total size
 *
 * HTTP/1.1 only (chunked does not exist in 1.0). Handlers serving HEAD get
 * headers and no body, as with the buffered path. */
void ca_http_resp_begin_chunked(ca_http_response_t *r, u16 status,
                                const char *content_type);

/* Pushes whatever is staged in the buffer out as one chunk. Called
 * automatically when the buffer fills; rarely needed by hand. Returns false if
 * the send failed (the response is then aborted). */
bool ca_http_resp_flush(ca_http_response_t *r);

/* ── Route table ──────────────────────────────────────────────────────────── */
typedef void (*ca_http_handler_fn)(const ca_http_request_t *req,
                                   ca_http_response_t      *resp);

typedef struct {
    const char        *uri;      /* e.g. "/status" — exact match */
    u8                 methods;  /* bitmask of accepted HTTP_METHOD_* */
    ca_http_handler_fn handler;
} ca_http_route_t;

/* ── One connection ───────────────────────────────────────────────────────────
 * Buffers of a connection being served. One per service (http, https), so the
 * two tasks never share them. STATIC storage, never a task stack: in chunked
 * mode a handler may hand `resp` to another thread (ex80 /cli). */
typedef struct {
    u8                 rx[HTTP_REQ_BUF_SIZE];
    u8                 tx[HTTP_RESP_BUF_SIZE];
    ca_http_response_t resp;
} ca_http_conn_t;

/* Reads one request from io, runs the route table given to
 * ca_http_service_start() and writes the response. The transport is left open:
 * closing it (TCP FIN, TLS close_notify) is the caller's job. */
void ca_http_serve_connection(ca_http_conn_t *c, const ca_http_io_t *io);

/* ── Service API ──────────────────────────────────────────────────────────── */

/* Starts the HTTP task and registers CLI.
 * routes/count: application route table — pointer must remain valid.
 * port: 0 → use HTTP_DEFAULT_PORT. */
void ca_http_service_start(const ca_http_route_t *routes, u8 count, u16 port);

/* Updates local IP after DHCP bind (call from on_dhcp_bind callback). */
void ca_http_update_local_ip(ca_ipv4_t ip);

#endif /* EN_HTTP_SERVICE */
