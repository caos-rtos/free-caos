// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

#ifndef SECURE_PIPE_SERVICE_H
#define SECURE_PIPE_SERVICE_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_AES_CHANNEL_SERVICE

#include "aes_channel_service.h"
#include "queue.h"

/*
 * Secure pipe: bidirectional encrypted message pipe between two tasks.
 *
 * Two ends (A, B), one queue per direction, one AES channel shared by both.
 * send() encrypts and queues the ciphertext toward the other end; recv()
 * dequeues, decrypts and copies the plaintext into the caller buffer.
 * Ciphertext and plaintext heap buffers never leave the service and are
 * always wiped on free: the caller owns nothing.
 *
 * Enabled by EN_AES_CHANNEL_SERVICE (same service, same flag).
 */

/* Return codes */
#define CA_SECURE_PIPE_OK         0
#define CA_SECURE_PIPE_ERR       -1   /* bad args, pipe not ready, heap full, encrypt fail */
#define CA_SECURE_PIPE_TIMEOUT   -2   /* queue full (send) / empty (recv) until timeout */
#define CA_SECURE_PIPE_DECRYPT   -3   /* tag mismatch or decrypt fail: message dropped */
#define CA_SECURE_PIPE_SIZE      -4   /* message larger than buffer: dropped, *out_len = size */

/* Plaintext length travels as 16 bit in the channel wire format */
#define CA_SECURE_PIPE_MAX_MSG    0xFFFFU

typedef enum {
    CA_SECURE_PIPE_A = 0U,
    CA_SECURE_PIPE_B = 1U,
} ca_secure_pipe_end_t;

typedef struct {
    ca_aes_channel_t ch;
    ca_queue_t       q[2];    /* q[e]: ciphertext waiting to be read by end e */
    bool             ready;
} ca_secure_pipe_t;

/*
 * Init: AES channel on ks_handle (from ca_keys_store_allocate) + two heap
 * queues of `depth` messages. p must start zeroed (static) or deinit-ed:
 * an initialized pipe is refused.
 */
i32  ca_secure_pipe_init(ca_secure_pipe_t *p,
                         ca_aes_mode_t mode, ca_aes_keysize_t keysize,
                         void *ks_handle, u32 depth);

/*
 * Encrypt len bytes and queue them toward the other end of `from`.
 * Blocks up to timeout_ms if the queue is full. Not from ISR.
 */
i32  ca_secure_pipe_send(ca_secure_pipe_t *p, ca_secure_pipe_end_t from,
                         const void *data, u32 len, u32 timeout_ms);

/*
 * Wait up to timeout_ms for a message at end `at`, decrypt it into buf.
 * *out_len = plaintext length. A message that does not fit in cap is
 * dropped (CA_SECURE_PIPE_SIZE, *out_len = its size). Not from ISR.
 */
i32  ca_secure_pipe_recv(ca_secure_pipe_t *p, ca_secure_pipe_end_t at,
                         void *buf, u32 cap, u32 *out_len, u32 timeout_ms);

/*
 * Wipe and free the queued messages, destroy the queues, close the AES
 * channel. No task may be inside send/recv on this pipe: a woken waiter
 * goes back to sleep on the destroyed queue, and a later init would wipe
 * a wait list that still holds it.
 */
void ca_secure_pipe_deinit(ca_secure_pipe_t *p);

#if EN_SHA256
/* ============================================================================
 * LINK MODE - encrypted point-to-point channel between TWO BOARDS
 * ============================================================================
 *
 * The pipe above joins two tasks of one board. The link joins two boards:
 * it seals and opens BUFFERS, and the application carries them (UDP in
 * app_3). Nothing here touches the network, so both ends can be exercised
 * on a single board.
 *
 * KEYS
 *   password + name (both 8..16 chars, the same on the two boards)
 *     master = PBKDF2-HMAC-SHA256(password, name, CA_SP_LINK_PBKDF2_ITER)
 *     K12 = HKDF(master, "caos-sp-link 1->2"), K21 = HKDF(master, "... 2->1")
 *   Role 1 sends with K12 and receives with K21, role 2 the opposite: a
 *   message cannot be reflected back to its sender. Only K12/K21 survive,
 *   in keys_store; password, name copy and master are wiped.
 *
 * HANDSHAKE (every boot, every peer reboot, every rekey)
 *   HELLO = [type:1][role:1][iv:12][nonce|echo encrypted:32][tag:16]
 *   type 1 starts a handshake, type 3 is the reply to one and echoes the
 *   nonce it answers. The starter pairs only with a reply to its CURRENT
 *   nonce; a reply is never replied to (else the two ends answer each other
 *   for ever); if both start, role 2 answers and role 1 waits.
 *   Each side sends a fresh random 16-byte nonce under its static key.
 *   With both nonces: S12 = HKDF(K12, salt = n1|n2), S21 = HKDF(K21, ...).
 *   Fresh session keys every time, so the data counter can restart at 1
 *   with no GCM nonce reuse, and a recording of an older session is
 *   worthless in the next one.
 *
 * DATA
 *   [type=2][counter:8][len:2][ciphertext padded to 16][tag:16]
 *   GCM nonce = 0000|counter (never reused within a session). AAD = the
 *   header + the caller's AAD (app_3: IP addresses and UDP ports), so the
 *   whole header is authenticated and a sealed payload cannot be moved to
 *   another flow. Anti-replay: 64-message window, updated only after the
 *   tag has verified.
 */

#define CA_SECURE_PIPE_REPLAY     -5   /* counter already seen, or too old  */
#define CA_SECURE_PIPE_NOT_UP     -6   /* no session yet: handshake first   */
#define CA_SECURE_PIPE_REPLY       1   /* accept(): send hello() now        */

#define CA_SP_LINK_PBKDF2_ITER     100000U   /* 2.25 s on H753 (ex66)        */
#define CA_SP_LINK_SECRET_MIN      8U        /* password and name length    */
#define CA_SP_LINK_SECRET_MAX      16U
#define CA_SP_LINK_HELLO_SIZE      (1U + 1U + 12U + 32U + 16U)    /* 62 */
#define CA_SP_LINK_DATA_HDR        (1U + 8U + 2U)                 /* 11 */
#define CA_SP_LINK_OVERHEAD_MAX    (CA_SP_LINK_DATA_HDR + 15U + 16U)
#define CA_SP_LINK_AAD_MAX         48U       /* caller AAD                  */
#define CA_SP_LINK_REKEY_MSGS      (1UL << 20)
#define CA_SP_LINK_REKEY_MS        (60UL * 60UL * 1000UL)

typedef struct {
    u32 hello_ok, hello_bad;        /* handshakes accepted / rejected       */
    u32 sessions;                   /* session keys derived                  */
    u32 sealed, opened;
    u32 tag_fail;                   /* forged, tampered or wrong key         */
    u32 replay;                     /* duplicate or out of the window        */
    u32 bad_format;                 /* wrong type, length, size              */
} ca_sp_link_stats_t;

typedef struct {
    u8            role;             /* 1 or 2                                */
    bool          ready;            /* init done                             */
    bool          up;               /* session keys derived                  */
    bool          my_fresh;         /* my_nonce sent, not paired yet         */
    bool          peer_pending;     /* peer nonce waiting for my hello()     */
    u8            my_nonce[16];
    u8            peer_nonce[16];
    void         *k_tx, *k_rx;      /* static direction keys (keys_store)    */
    void         *s_tx, *s_rx;      /* session AES contexts (keys_store), set
                                     * up once per session, only the nonce
                                     * changes per message                   */
    ca_aes_ctx_t *ctx;              /* heap, set up per message              */
    ca_mutex_t    mutex;
    u64           tx_ctr;           /* last counter sent                     */
    u64           rx_top;           /* highest counter accepted              */
    u64           rx_win;           /* bit i = rx_top - i already accepted   */
    u32           session_ms;       /* ca_os_tick_ms() at derivation         */
    ca_sp_link_stats_t st;
} ca_sp_link_t;

/*
 * Derive K12/K21 from password + name (PBKDF2, takes seconds) and store them
 * in keys_store: two slots of 32 bytes (static keys) and two of
 * sizeof(ca_aes_ctx_t) (the session contexts, set up once per session): the
 * caller must have sized ca_keys_store_init() for them. role = 1 or 2.
 * The caller should wipe its own copy of the password afterwards.
 */
i32  ca_sp_link_init(ca_sp_link_t *l, u8 role,
                     const char *password, const char *name);

/* Fresh nonce -> HELLO into out (CA_SP_LINK_HELLO_SIZE bytes). reply =
 * false to start (or retry, or rekey), true to answer accept()'s REPLY: then
 * the peer's waiting nonce pairs with the new one and the session is derived. */
i32  ca_sp_link_hello(ca_sp_link_t *l, bool reply, u8 *out, u32 cap, u32 *out_len);

/* The peer's HELLO. OK = session derived; REPLY = answer with hello(reply =
 * true) (the peer started or restarted); REPLAY = a reply to a nonce already
 * paired, ignored; DECRYPT = not from the peer's key. */
i32  ca_sp_link_accept(ca_sp_link_t *l, const u8 *in, u32 len);

/* Seal plaintext with optional AAD (<= CA_SP_LINK_AAD_MAX) into out.
 * out needs len + CA_SP_LINK_OVERHEAD_MAX bytes at most. */
i32  ca_sp_link_seal(ca_sp_link_t *l, const u8 *aad, u32 aad_len,
                     const u8 *plain, u32 len, u8 *out, u32 cap, u32 *out_len);

/* Verify and open: same AAD as the sender's, else DECRYPT. REPLAY for a
 * counter already accepted or older than the window. */
i32  ca_sp_link_open(ca_sp_link_t *l, const u8 *aad, u32 aad_len,
                     const u8 *in, u32 len, u8 *out, u32 cap, u32 *out_len);

bool ca_sp_link_is_up(const ca_sp_link_t *l);
/* Session older than CA_SP_LINK_REKEY_MS or CA_SP_LINK_REKEY_MSGS sealed:
 * the application starts a new handshake with hello(). */
bool ca_sp_link_need_rekey(const ca_sp_link_t *l);

/* Wipe and free keys and context. */
void ca_sp_link_deinit(ca_sp_link_t *l);
#endif /* EN_SHA256 */

#endif /* EN_AES_CHANNEL_SERVICE */
#endif /* SECURE_PIPE_SERVICE_H */
