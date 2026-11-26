// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    vnc_service.h
 * @brief   VNC server: shows an fb framebuffer to one RFB client over TCP
 ******************************************************************************
 * The service owns the framebuffer it shows: its task takes it with
 * ca_fb_take() (fb.h), so the framebuffer is freed when the service is
 * destroyed. The application keeps drawing through the fb handle; the
 * service finds what changed by itself (tile hashes, vnc_proto.h) and sends
 * only that, in the format the client asks for (8/16/32 bpp, Raw).
 *
 * One client at a time: a second connection is closed at once. Start it
 * after the board has its IP address (DHCP bound): the listen socket takes
 * the local IP at start.
 *
 * Security (cfg.security), decided once at init:
 *   CA_VNC_SECURITY_AUTO  (default) TLS where the part has AES in hardware
 *                         (ca_aes_is_hw(), from the CMSIS header - no list of
 *                         MCUs) and EN_VNC_TLS is built; elsewhere plain RFB,
 *                         and then the session is forced VIEW ONLY
 *   CA_VNC_SECURITY_TLS   always TLS (slow where AES is software)
 *   CA_VNC_SECURITY_NONE  plain RFB, as asked: trusted networks only
 * TLS is VeNCrypt X509None: the whole session over the caOS TLS 1.3
 * (AES-256-GCM-SHA384 when the client offers it), server identity
 * cfg.cert_der + cfg.private_key (P-256, copied into keys_store). Viewers
 * (TigerVNC, Remmina) ask once to accept a self-signed certificate.
 *
 * Input. The application registers on_key / on_pointer; every event says
 * where it came from:
 *   CA_VNC_SRC_CLIENT  the VNC client - only if cfg.client_input is true,
 *                      otherwise the session is view only and its events
 *                      are consumed and dropped (the default)
 *   CA_VNC_SRC_LOCAL   local hardware (keys, encoder, touch) through
 *                      ca_vnc_inject_key() / ca_vnc_inject_pointer()
 *   CA_VNC_SRC_REMOTE  another connection the application trusts (SSH,
 *                      HTTPS, its own protocol), injected the same way
 * So the application has one input path whatever the source, and decides
 * per source. Client events are delivered in the service task, injected
 * ones in the caller's task. Task context only.
 *
 * Lifecycle: init (configuration) -> start (task) -> stop (client dropped,
 * listening paused, task kept) -> start again, or destroy (task ends,
 * framebuffer and buffers freed). Stop and destroy are cooperative: the task
 * notices within VNC_POLL_MS.
 */
#ifndef VNC_SERVICE_H
#define VNC_SERVICE_H

#include "common.h"
#include "os_config.h"

#if EN_VNC_SERVICE && EN_NET && EN_TCP_SERVICE && EN_FB

#include "fb.h"
#include "vnc_proto.h"

typedef enum {
    CA_VNC_SRC_CLIENT = 0,      /* the VNC client (cfg.client_input) */
    CA_VNC_SRC_LOCAL,           /* local hardware */
    CA_VNC_SRC_REMOTE           /* another trusted connection */
} ca_vnc_src_t;

/* keysym: X11 keysym (RFC 6143 7.5.4); buttons: bit 0 left, 1 middle,
   2 right, 3-4 wheel up/down */
typedef void (*ca_vnc_on_key_t)(ca_vnc_src_t src, bool down, u32 keysym);
typedef void (*ca_vnc_on_pointer_t)(ca_vnc_src_t src, u8 buttons, u16 x, u16 y);

/* X509Plain login (TLS sessions): the client sends user + password inside
 * TLS; the board keeps only PBKDF2-HMAC-SHA256(password, salt, iterations).
 * Generated on the PC: tools/gen_vnc_cred.py <user> <password>. */
typedef struct {
    const char *user;                   /* 1..CA_VNC_USER_MAX characters */
    u8          salt[16];
    u32         iterations;
    u8          hash[32];
} ca_vnc_cred_t;

typedef enum {
    CA_VNC_SECURITY_AUTO = 0,           /* TLS if AES is in hardware, else view only */
    CA_VNC_SECURITY_TLS,                /* always TLS */
    CA_VNC_SECURITY_NONE                /* plain RFB */
} ca_vnc_security_t;

typedef struct {
    ca_fb_t             fb;             /* shown; taken by the service task */
    ca_vnc_security_t   security;       /* 0 = AUTO */
    const u8           *cert_der;       /* TLS: X.509 DER of the P-256 key */
    u32                 cert_len;
    const u8           *private_key;    /* TLS: 32 bytes, copied at init */
    u16                 port;           /* 0 = VNC_DEFAULT_PORT */
    const char         *name;           /* desktop name, NULL = "caOS" */
    bool                client_input;   /* false = view only */
    const ca_vnc_cred_t *cred;          /* TLS: required (X509Plain login), else init fails */
    ca_vnc_on_key_t     on_key;         /* NULL = keys dropped */
    ca_vnc_on_pointer_t on_pointer;     /* NULL = pointer dropped */
} ca_vnc_cfg_t;

typedef struct {
    bool     running;           /* task alive and listening */
    bool     connected;         /* a client is in session */
    u32      sessions;          /* clients served */
    u32      refused;           /* second clients closed */
    u32      updates, rects, pixels;
    u32      bytes_tx;
    u32      keys, pointers;    /* client events received (dropped or not) */
    u32      auth_ok, auth_fail;  /* X509Plain logins */
    u32      auth_backoff;      /* clients closed during the wait after a failure */
    u32      auth_ms;           /* time of the last password check (PBKDF2) */
    u8       bpp;               /* client's format, 0 = no client */
    bool     tls;               /* sessions run over TLS (decided at init) */
    u16      suite;             /* last negotiated TLS suite, 0 = none */
    u32      tls_ok, tls_fail;  /* TLS handshakes */
    u8       tls_alert;         /* alert of the last failed handshake */
    const char *last_close;     /* why the last session ended */
} ca_vnc_stats_t;

/** @brief Stores the configuration. false if already running or fb invalid */
bool ca_vnc_service_init(const ca_vnc_cfg_t *cfg);

/** @brief Creates the task (first time) or resumes listening after a stop */
bool ca_vnc_service_start(void);

/** @brief Drops the client and stops listening; the task and the fb stay */
void ca_vnc_service_stop(void);

/** @brief Stops, ends the task, frees framebuffer and buffers. true if gone */
bool ca_vnc_service_destroy(void);

/** @brief Client input on (events to the callbacks) or off (view only), at any time */
void ca_vnc_service_set_client_input(bool on);

/** @brief An input event from local hardware or another connection */
bool ca_vnc_inject_key(ca_vnc_src_t src, bool down, u32 keysym);
bool ca_vnc_inject_pointer(ca_vnc_src_t src, u8 buttons, u16 x, u16 y);

void ca_vnc_service_stats(ca_vnc_stats_t *st);

#endif /* EN_VNC_SERVICE */
#endif /* VNC_SERVICE_H */
