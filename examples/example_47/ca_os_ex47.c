// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 47 -- Keystore service: makekey + encrypted ping-pong
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *
 * CLI:
 *   makekey <pwd1> <pwd2> <N>  generates key block → flash (last 8 sectors)
 *   start   <pwd2> <N>         decrypts block, starts ping-pong with key N
 *
 * REQUIREMENTS os_config.h:
 *   EN_AES 1, EN_RNG 1, EN_HASH 1, EN_FLASH 1
 *   EN_KEYS_STORE 1, EN_AES_CHANNEL_SERVICE 1, EN_KEYSTORE_SERVICE 1
 */

#include "caos.h"

#if EN_AES && EN_RNG && EN_HASH && EN_FLASH && \
    EN_KEYS_STORE && EN_AES_CHANNEL_SERVICE && EN_KEYSTORE_SERVICE


#define PING_INTERVAL_MS  1000U
#define PING_COUNT        20U
#define NUM_KEYS          16U
/* Last physical sector/page, derived at runtime: sector layout (count AND
 * size) differs per family (F4xx: 12×16/64/128KB, H7xx: 16×128KB, H5xx/U5xx:
 * 256×8KB) so a fixed index like the old "sector 6" (valid on H753, 128KB
 * pages) can land INSIDE the code area on a small-page family — U575 pages
 * are 8KB and .text alone is ~87KB, sector 6 = offset 48KB was mid-firmware:
 * erasing it corrupted the running binary (board bricked, HW confirmed).
 * The last sector is always far from .text regardless of family. */
#define KEY_FLASH_SECTOR  ((u8)(ca_flash_hw_num_sectors() - 1U))

/* ============================================================================
 * PING-PONG SECURE PIPE (same exchange as ex46)
 * ========================================================================== */

#define PIPE_DEPTH  4U

static ca_secure_pipe_t s_pipe;
static void            *s_traffic_key_ptr = NULL;
static volatile bool    s_running         = false;
/* Alice finished: Bob, the only task that can still be inside recv,
 * closes the pipe. start is refused until he has. */
static volatile bool    s_done            = false;

/* ============================================================================
 * HELPER: derives piece (16 bytes) from password + IV via SHA256 x N
 * piece = SHA256^N(IV || pad16(pwd)) [:16]
 * ========================================================================== */

static void pad16(const char *pwd, u8 out[16]) {
    u32 len = 0;
    while (pwd[len] && len < 16) len++;
    ca_memcpy(out, pwd, len);
    if (len < 16) ca_memset(out + len, 0, 16 - len);
}

static bool derive_piece(const u8 IV[16], const char *pwd,
                         u32 n_evolve, u8 piece[16],
                         const char *tag) {
    u8 seed[32];
    pad16(pwd, seed + 16);
    ca_memcpy(seed, IV, 16);

    ca_stdout_printf_n("[%s] IV:", tag);             ca_hexdump((u8 *)IV,  16);
    ca_stdout_printf_n("[%s] pwd_pad:", tag);        ca_hexdump(seed + 16, 16);
    ca_stdout_printf_n("[%s] seed(IV||pwd):", tag);  ca_hexdump(seed,      32);

    u8  digest[32];
    u32 dlen = 32;
    if (!ca_hash_compute(CA_HASH_ALGO_SHA256, seed, 32, digest, &dlen))
        return false;
    ca_stdout_printf_n("[%s] SHA256 iter 1:", tag);  ca_hexdump(digest, 32);

    for (u32 i = 1; i < n_evolve; i++) {
        dlen = 32;
        if (!ca_hash_compute(CA_HASH_ALGO_SHA256, digest, 32, digest, &dlen))
            return false;
        ca_stdout_printf_n("[%s] SHA256 iter %u:", tag, (unsigned)(i + 1));
        ca_hexdump(digest, 32);
    }

    ca_memcpy(piece, digest, 16);
    ca_stdout_printf_n("[%s] piece[:16]:", tag);     ca_hexdump(piece, 16);

    ca_memzero(seed,   sizeof(seed));
    ca_memzero(digest, sizeof(digest));
    return true;
}

/* ============================================================================
 * MAKEKEY — builds and writes the block to flash
 * ========================================================================== */

static bool do_makekey(const char *pwd1, const char *pwd2, u32 n_evolve) {
    /* --- random IV --- */
    u8 IV[16];
    if (!ca_rng_fill(IV, 16)) {
        CLI_PUT_N("makekey: RNG fail"); return false;
    }

    ca_stdout_put_n("[makekey] IV random:");
    ca_hexdump(IV, 16);

    /* --- piece1 = SHA256^N(IV||pwd1) [:16] --- */
    u8 piece1[16];
    if (!derive_piece(IV, pwd1, n_evolve, piece1, "piece1")) {
        CLI_PUT_N("makekey: piece1 derive fail"); return false;
    }

    /* --- piece2 = SHA256^N(IV||pwd2) [:16] --- */
    u8 piece2[16];
    if (!derive_piece(IV, pwd2, n_evolve, piece2, "piece2")) {
        CLI_PUT_N("makekey: piece2 derive fail"); return false;
    }

    /* --- generates NUM_KEYS random AES-256 keys ---
     * static, not on the stack: the four big buffers in this function add up to
     * ~3.3 KB, and the CLI task runs on STACK_MEDIUM (2 KB, fixed in cli.c for
     * every example). On the stack they overflowed straight past the guard band
     * into the next task's stack - the MPU reported it as a CROSS-TASK WRITE
     * into 'alice', not as an overflow, because the jump was ~1.3 KB wide.
     * makekey runs once, from the CLI task only: no reentrancy to preserve. */
    static ca_flash_key_entry_t keys[NUM_KEYS];
    for (u32 i = 0; i < NUM_KEYS; i++) {
        keys[i].key_id = (u16)i;
        ca_memset(keys[i].label, 0, KSTORE_SVC_LABEL_SIZE);
        ca_snprintf((char *)keys[i].label, KSTORE_SVC_LABEL_SIZE, "k%02u", (unsigned)i);
        if (!ca_rng_fill(keys[i].key, 32)) {
            CLI_PUT_N("makekey: RNG key fail"); return false;
        }
        keys[i].start = 0;
        keys[i].stop  = 0;
    }

    /* --- CYPHER_KEY = SHA256^N(IV||piece1||piece2) --- */
    u8 kdf_seed[KSTORE_SVC_IV_SIZE + KSTORE_SVC_P1_SIZE + KSTORE_SVC_P2_SIZE];
    ca_memcpy(kdf_seed,                                            IV,     16);
    ca_memcpy(kdf_seed + 16,                                       piece1, 16);
    ca_memcpy(kdf_seed + 32,                                       piece2, 16);

    u8  cypher_key[32];
    u32 dlen = 32;
    ca_stdout_put_n("[makekey] kdf_seed(IV||p1||p2):");
    ca_hexdump(kdf_seed, 48);

    if (!ca_hash_compute(CA_HASH_ALGO_SHA256, kdf_seed, 48, cypher_key, &dlen)) {
        CLI_PUT_N("makekey: KDF fail"); return false;
    }
    ca_stdout_put_n("[makekey] CYPHER_KEY iter 1:"); ca_hexdump(cypher_key, 32);
    for (u32 i = 1; i < n_evolve; i++) {
        dlen = 32;
        if (!ca_hash_compute(CA_HASH_ALGO_SHA256, cypher_key, 32, cypher_key, &dlen)) {
            CLI_PUT_N("makekey: KDF iter fail"); return false;
        }
        ca_stdout_printf_n("[makekey] CYPHER_KEY iter %u:", (unsigned)(i + 1));
        ca_hexdump(cypher_key, 32);
    }
    /* --- random AES-GCM nonce (12 bytes) --- */
    u8 gcm_nonce[12];
    if (!ca_rng_fill(gcm_nonce, 12)) {
        CLI_PUT_N("makekey: nonce RNG fail"); return false;
    }
    ca_stdout_put_n("[makekey] gcm_nonce:"); ca_hexdump(gcm_nonce, 12);

    /* --- builds cleartext header --- */
    u32 payload_plain_len = NUM_KEYS * sizeof(ca_flash_key_entry_t); /* 16*48=768 */
    u32 payload_enc_len   = (payload_plain_len + 15U) & ~15U;     /* ceil to 16 */
    u32 total_size        = sizeof(ca_flash_key_block_hdr_t)
                          + payload_enc_len
                          + KSTORE_SVC_TAG_SIZE;

    ca_flash_key_block_hdr_t hdr;
    hdr.size     = total_size;
    ca_memcpy(hdr.IV,        IV,        16);
    hdr.n_evolve = n_evolve;
    ca_memcpy(hdr.p1,        piece1,    16);
    hdr.n_key    = (u16)NUM_KEYS;
    ca_memcpy(hdr.gcm_nonce, gcm_nonce, 12);

    /* --- plaintext payload (padded) --- */
    static u8 plain_buf[768 + 16]; /* max payload_enc_len - static, see keys[] */
    ca_memset(plain_buf, 0, sizeof(plain_buf));
    ca_memcpy(plain_buf, keys, payload_plain_len);

    /* --- AES-GCM encrypt --- */
    static u8 enc_buf[768 + 16];   /* static, see keys[] */
    u8 tag[16];
    ca_aes_ctx_t ctx;
    if (!ca_aes_ctx_setup(&ctx, CA_AES_MODE_GCM, CA_AES_KEY_256, cypher_key, gcm_nonce)) {
        CLI_PUT_N("makekey: AES ctx fail"); return false;
    }
    if (!ca_aes_gcm_encrypt(&ctx,
                            (const u8 *)&hdr, sizeof(hdr),
                            plain_buf, enc_buf, payload_enc_len,
                            tag)) {
        CLI_PUT_N("makekey: AES-GCM encrypt fail"); return false;
    }
    ca_memzero(cypher_key, sizeof(cypher_key));
    ca_memzero(plain_buf,  sizeof(plain_buf));

    /* --- dedicated flash sector (bank1, tested) --- */
    u32 flash_addr  = ca_flash_get_sector_addr(KEY_FLASH_SECTOR);
    u32 flash_avail = ca_flash_get_sector_size(KEY_FLASH_SECTOR);
    if (!flash_addr || total_size > flash_avail) {
        CLI_PUT_N("makekey: invalid flash sector"); return false;
    }

    /* assembles hdr + enc + tag into a single 32-byte aligned buffer */
    u32 write_len = (total_size + 31U) & ~31U;
    static u8 flash_buf[sizeof(ca_flash_key_block_hdr_t) + 768 + 16 + 32]; /* static, see keys[] */
    ca_memset(flash_buf, 0xFF, sizeof(flash_buf));
    ca_memcpy(flash_buf,                              &hdr,    sizeof(hdr));
    ca_memcpy(flash_buf + sizeof(hdr),                enc_buf, payload_enc_len);
    ca_memcpy(flash_buf + sizeof(hdr) + payload_enc_len, tag,  16);

    ca_stdout_printf_n("[makekey] write %u byte @ 0x%08X",
                       (unsigned)write_len, (unsigned)flash_addr);

    /* erase + write blocking (path tested with flash test on ex23) */
    if (!ca_flash_erase_sector_num(KEY_FLASH_SECTOR)) {
        CLI_PUT_N("makekey: erase fail"); return false;
    }
    if (!ca_flash_hw_unlock()) {
        CLI_PUT_N("makekey: unlock fail"); return false;
    }
    bool wr_ok = ca_flash_hw_write(flash_addr, flash_buf, write_len);
    ca_flash_hw_lock();
    if (!wr_ok) {
        CLI_PUT_N("makekey: write fail"); return false;
    }

    /* --- init keystore service --- */
    if (ca_keystore_service_init(flash_addr) != 0) {
        CLI_PUT_N("makekey: keystore_service_init fail"); return false;
    }

    CLI_PRINTF_N("makekey: OK - %u bytes to flash @ 0x%08X, %u keys",
                 (unsigned)total_size,
                 (unsigned)flash_addr,
                 (unsigned)NUM_KEYS);
    return true;
}

/* ============================================================================
 * START — decrypts block, starts ping-pong with key[N]
 * ========================================================================== */

static bool do_start(const char *pwd2, u32 key_idx) {
    if (key_idx >= NUM_KEYS) {
        CLI_PRINTF_N("start: key_idx must be 0..%u", (unsigned)(NUM_KEYS-1));
        return false;
    }

    u32 flash_addr = ca_flash_get_sector_addr(KEY_FLASH_SECTOR);

    /* re-init if necessary */
    if (!ca_keystore_service_is_active()) {
        if (ca_keystore_service_init(flash_addr) != 0) {
            CLI_PUT_N("start: keystore_service_init fail"); return false;
        }
    }

    /* reads IV from header in flash to derive piece2 */
    ca_flash_key_block_hdr_t hdr;
    ca_memcpy(&hdr, (const void *)flash_addr, sizeof(hdr));

    ca_stdout_put_n("[start] IV from flash:");
    ca_hexdump(hdr.IV, 16);

    u8 piece2[16];
    if (!derive_piece(hdr.IV, pwd2, hdr.n_evolve, piece2, "start/p2")) {
        CLI_PUT_N("start: piece2 derive fail"); return false;
    }

    if (ca_keystore_service_unlock(piece2) != 0) {
        CLI_PUT_N("start: unlock fail (wrong password?)"); return false;
    }
    ca_memzero(piece2, sizeof(piece2));

    /* retrieves pointer to key N - static for the same reason as the buffers in
     * do_makekey(): 20 entries are ~480 bytes, and on top of the SHA chain that
     * derive_piece() runs below it pushed this task past 75% of its 2 KB. */
    static ca_secure_key_entry_t info[MAX_SECURE_KEYS];
    u8 cnt = MAX_SECURE_KEYS;
    ca_keys_store_get_keys_info(info, &cnt);
    if (key_idx >= cnt) {
        CLI_PRINTF_N("start: only %u keys available", (unsigned)cnt);
        ca_keystore_service_lock(); return false;
    }
    s_traffic_key_ptr = (void *)info[key_idx].base_address;

    /* secure pipe on the selected key */
    if (ca_secure_pipe_init(&s_pipe, CA_AES_MODE_GCM, CA_AES_KEY_256,
                            s_traffic_key_ptr, PIPE_DEPTH) != CA_SECURE_PIPE_OK) {
        CLI_PUT_N("start: secure_pipe_init fail");
        ca_keystore_service_lock(); return false;
    }

    s_running = true;
    CLI_PRINTF_N("start: OK - key[%u] loaded, starting ping-pong", (unsigned)key_idx);
    return true;
}

/* ============================================================================
 * CLI — state machine for makekey and start
 * ========================================================================== */

typedef enum {
    CLI_IDLE = 0,
    CLI_MAKEKEY_PWD1,
    CLI_MAKEKEY_PWD2,
    CLI_MAKEKEY_NEVOLVE,
    CLI_START_PWD2,
    CLI_START_KIDX,
} cli_state_t;

static cli_state_t s_cli_state  = CLI_IDLE;
static char        s_pwd1[32]   = {0};
static char        s_pwd2[32]   = {0};
static u32         s_n_evolve   = 1;

/* ---------------------------------------------------------------------------
 * CRYPTO WORKER
 *
 * makekey and start are long jobs with a deep call chain: SHA-256 runs in
 * software on this part (no HASH accelerator), and derive_piece() iterates it
 * N times. Run from the CLI handler they pushed the CLI task - STACK_MEDIUM,
 * 2 KB, fixed in cli.c for every example - to 75% watermark, after the big
 * buffers had already been moved to .bss. A CLI handler should stay shallow:
 * the work belongs to a task sized for it.
 *
 * The worker owns STACK_LARGE (4 KB) and does nothing until the CLI posts a
 * request, so the cost is one task, not one task per command.
 * ------------------------------------------------------------------------- */
typedef enum { JOB_NONE = 0, JOB_MAKEKEY, JOB_START } crypto_job_t;

static volatile crypto_job_t s_job = JOB_NONE;
static u32                   s_job_idx;      /* key index, for JOB_START */
static ca_semaphore_t           s_job_sem;

static u32 task_crypto(void *arg)
{
    (void)arg;
    while (1) {
        ca_sem_wait(&s_job_sem);
        switch (s_job) {
        case JOB_MAKEKEY:
            do_makekey(s_pwd1, s_pwd2, s_n_evolve);
            ca_memzero(s_pwd1, sizeof(s_pwd1));
            ca_memzero(s_pwd2, sizeof(s_pwd2));
            break;
        case JOB_START:
            do_start(s_pwd2, s_job_idx);
            ca_memzero(s_pwd2, sizeof(s_pwd2));
            break;
        default:
            break;
        }
        s_job = JOB_NONE;
    }
    return 0;
}

/* Hand the job over and return at once: the CLI task must not do the work. */
static void crypto_post(crypto_job_t job, u32 idx)
{
    if (s_job != JOB_NONE) {
        CLI_PUT_N("crypto: busy, retry in a moment");
        return;
    }
    s_job_idx = idx;
    s_job     = job;
    ca_sem_post(&s_job_sem);
}

/* ----------------------------------------------------------------------------
 * Argument parsing for the one-line command forms.
 *
 * The header of this file has always documented "makekey <pwd1> <pwd2> <N>",
 * but only the interactive prompt sequence existed, so anything driving this
 * example from a script - the hw_test bench included - hung waiting for a
 * prompt it had not been told about. Both forms work now: arguments when they
 * are given, prompts when they are not.
 * ------------------------------------------------------------------------- */

static const char *ex47_word(const char *s, char *out, u32 cap) {
    u32 i = 0;
    s = ca_skip_sp(s);
    while (*s != '\0' && *s != ' ' && i + 1U < cap)
        out[i++] = *s++;
    out[i] = '\0';
    return ca_skip_sp(s);
}

static bool ex47_u32(const char *s, u32 *out) {
    u32 v = 0;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') v = v * 10U + (u32)(*s++ - '0');
    *out = v;
    return true;
}

static bool ex47_arg3(const char *a, char *w1, u32 c1, char *w2, u32 c2, u32 *n) {
    a = ex47_word(a, w1, c1);
    if (w1[0] == '\0') return false;
    a = ex47_word(a, w2, c2);
    if (w2[0] == '\0') return false;
    return ex47_u32(a, n);
}

static bool ex47_arg2(const char *a, char *w1, u32 c1, u32 *n) {
    a = ex47_word(a, w1, c1);
    if (w1[0] == '\0') return false;
    return ex47_u32(a, n);
}

static bool ex47_cli_handler(c_str *pcmd) {
    const char *p = ca_skip_sp((*pcmd)->data);

    /* multi-step input: intercepts input when the state machine is active */
    switch (s_cli_state) {
    case CLI_MAKEKEY_PWD1:
        ca_strncpy(s_pwd1, p, sizeof(s_pwd1) - 1);
        CLI_PUT_N("makekey: password2:");
        s_cli_state = CLI_MAKEKEY_PWD2;
        return true;

    case CLI_MAKEKEY_PWD2:
        ca_strncpy(s_pwd2, p, sizeof(s_pwd2) - 1);
        CLI_PUT_N("makekey: n_evolutions (1..16):");
        s_cli_state = CLI_MAKEKEY_NEVOLVE;
        return true;

    case CLI_MAKEKEY_NEVOLVE: {
        u32 n = 0;
        for (const char *c = p; *c >= '0' && *c <= '9'; c++)
            n = n * 10 + (*c - '0');
        if (n < 1 || n > 16) { CLI_PUT_N("makekey: value must be 1..16"); return true; }
        s_n_evolve  = n;
        s_cli_state = CLI_IDLE;
        crypto_post(JOB_MAKEKEY, 0);   /* runs in task_crypto, not here */
        return true;
    }

    case CLI_START_PWD2:
        ca_strncpy(s_pwd2, p, sizeof(s_pwd2) - 1);
        CLI_PUT_N("start: key index (0..15):");
        s_cli_state = CLI_START_KIDX;
        return true;

    case CLI_START_KIDX: {
        u32 idx = 0;
        for (const char *c = p; *c >= '0' && *c <= '9'; c++)
            idx = idx * 10 + (*c - '0');
        s_cli_state = CLI_IDLE;
        crypto_post(JOB_START, idx);   /* runs in task_crypto, not here */
        return true;
    }

    default:
        break;
    }

    /* top-level commands */
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("makekey   : [<pwd1> <pwd2> <N>] generate the key block in flash");
        CLI_PUT_N("start     : [<pwd2> <N>] decrypt and start ping-pong");
        return false;
    }

    if (ca_strncmp(p, "makekey", 7) == 0) {
        if (s_cli_state != CLI_IDLE) {
            CLI_PUT_N("ex47: command in progress, please wait"); return true;
        }
        /* One-line form first: "makekey <pwd1> <pwd2> <N>", which is what the
         * header of this file has always documented. Without arguments the
         * prompt sequence below still works, for typing it by hand. */
        const char *a = ca_skip_sp(p + 7);
        if (*a != '\0') {
            char w1[32], w2[32];
            u32  n = 1;
            if (!ex47_arg3(a, w1, sizeof(w1), w2, sizeof(w2), &n)) {
                CLI_PUT_N("makekey: usage: makekey <pwd1> <pwd2> <N>");
                return true;
            }
            if (n < 1U || n > 16U) {
                CLI_PUT_N("makekey: n_evolutions must be 1..16");
                return true;
            }
            ca_strncpy(s_pwd1, w1, sizeof(s_pwd1) - 1);
            ca_strncpy(s_pwd2, w2, sizeof(s_pwd2) - 1);
            s_n_evolve  = n;
            s_cli_state = CLI_IDLE;
            crypto_post(JOB_MAKEKEY, 0);   /* runs in task_crypto, not here */
            return true;
        }
        CLI_PUT_N("makekey: password1:");
        s_cli_state = CLI_MAKEKEY_PWD1;
        return true;
    }

    if (ca_strncmp(p, "start", 5) == 0) {
        if (s_cli_state != CLI_IDLE) {
            CLI_PUT_N("ex47: command in progress, please wait"); return true;
        }
        if (s_running || s_done) {
            CLI_PUT_N("start: ping-pong already running"); return true;
        }
        /* "start <pwd2> <N>", same reasoning as makekey above. */
        const char *a = ca_skip_sp(p + 5);
        if (*a != '\0') {
            char w1[32];
            u32  n = 0;
            if (!ex47_arg2(a, w1, sizeof(w1), &n)) {
                CLI_PUT_N("start: usage: start <pwd2> <N>");
                return true;
            }
            if (n > 15U) {
                CLI_PUT_N("start: key index must be 0..15");
                return true;
            }
            ca_strncpy(s_pwd2, w1, sizeof(s_pwd2) - 1);
            s_cli_state = CLI_IDLE;
            crypto_post(JOB_START, n);     /* runs in task_crypto, not here */
            return true;
        }
        CLI_PUT_N("start: password2:");
        s_cli_state = CLI_START_PWD2;
        return true;
    }

    return false;
}

static ca_cli_node_t s_ex47_cli = { ex47_cli_handler, NULL };

/* ============================================================================
 * PING-PONG TASKS (same as ex46, wait for s_running)
 * ========================================================================== */

static u32 task_hb(void *arg) {
    (void)arg;
    while (1) { ca_led_toggle(1U); ca_os_sleep(500); }
    return 0;
}

static u32 task_alice(void *arg) {
    (void)arg;
    while (!s_running) ca_os_sleep(200);
    ca_os_sleep(300);
    ca_stdout_put_n("=== EX47: AES-GCM ping-pong (keystore, secure pipe) ===");

    u32 ok_count = 0;
    for (u32 ping = 0; ping < PING_COUNT; ping++) {
        char plain[64];
        u32 plen = (u32)ca_snprintf(plain, sizeof(plain),
                                    "PING #%u from Alice", (unsigned)ping);
        i32 rc = ca_secure_pipe_send(&s_pipe, CA_SECURE_PIPE_A, plain, plen, 2000);
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_printf_n("Alice: %s #%u",
                               rc == CA_SECURE_PIPE_TIMEOUT ? "send FAIL" : "enc FAIL",
                               (unsigned)ping);
            ca_os_sleep(PING_INTERVAL_MS); continue;
        }
        ca_stdout_printf_n("Alice TX: '%s'", plain);

        char dec[64];
        u32  dec_len = 0;
        rc = ca_secure_pipe_recv(&s_pipe, CA_SECURE_PIPE_A, dec, sizeof(dec) - 1U,
                                 &dec_len, 5000);
        if (rc == CA_SECURE_PIPE_TIMEOUT) {
            ca_stdout_put_n("Alice: echo timeout"); continue;
        }
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_put_n("Alice: dec FAIL"); continue;
        }
        dec[dec_len] = '\0';

        char expected[64];
        u32 exp_len = (u32)ca_snprintf(expected, sizeof(expected),
                                       "PONG #%u from Bob", (unsigned)ping);
        bool match = (dec_len == exp_len) && (ca_memcmp(dec, expected, dec_len) == 0);
        ca_stdout_printf_n("Alice RX: '%s' - %s", dec, match ? "OK" : "MISMATCH");
        if (match) ok_count++;
        ca_os_sleep(PING_INTERVAL_MS);
    }

    ca_stdout_printf_n("Alice: %u/%u ping OK", (unsigned)ok_count, (unsigned)PING_COUNT);
    ca_stdout_put_n("Alice: test done.");
    s_done    = true;
    s_running = false;
    return 0;
}

static u32 task_bob(void *arg) {
    (void)arg;
    while (1) {
        if (!s_running) {
            if (s_done) {
                ca_secure_pipe_deinit(&s_pipe);
                s_done = false;
            }
            ca_os_sleep(200); continue;
        }

        char dec[64];
        u32  dec_len = 0;
        i32 rc = ca_secure_pipe_recv(&s_pipe, CA_SECURE_PIPE_B, dec, sizeof(dec) - 1U,
                                     &dec_len, 2000);
        if (rc == CA_SECURE_PIPE_TIMEOUT) continue;
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_put_n("Bob: dec FAIL"); continue;
        }
        dec[dec_len] = '\0';
        ca_stdout_printf_n("Bob RX: '%s'", dec);

        u32 ping_num = 0;
        for (u32 i = 0; i < dec_len; i++) {
            if (dec[i] == '#') {
                for (u32 j = i+1; j < dec_len && dec[j] >= '0' && dec[j] <= '9'; j++)
                    ping_num = ping_num * 10 + (u32)(dec[j] - '0');
                break;
            }
        }

        char pong[64];
        u32 pong_len = (u32)ca_snprintf(pong, sizeof(pong),
                                        "PONG #%u from Bob", (unsigned)ping_num);
        rc = ca_secure_pipe_send(&s_pipe, CA_SECURE_PIPE_B, pong, pong_len, 2000);
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_put_n(rc == CA_SECURE_PIPE_TIMEOUT ? "Bob: reply FAIL" : "Bob: enc FAIL");
            continue;
        }
        ca_stdout_printf_n("Bob TX: '%s'", pong);
    }
    return 0;
}

/* ============================================================================
 * INIT
 * ========================================================================== */

void ca_os_configure(void) {
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(1024);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_ex47_cli);
    )

    ca_led_init();
    ca_led_off(1U);

    ca_flash_init();
    ca_flash_irq_init();
    if (!ca_aes_init())  CA_SAFE_FAIL("ca_aes_init");
    if (!ca_hash_init()) CA_SAFE_FAIL("ca_hash_init");
    if (!ca_rng_init())  CA_SAFE_FAIL("ca_rng_init");

    if (ca_keys_store_init(MAX_SECURE_KEYS) != 0)
        CA_SAFE_FAIL("ca_keys_store_init");
}

void ca_os_main(void) {
    /* crypto worker: STACK_LARGE because the software SHA-256 chain is deep,
     * and LEVEL5 (~25% CPU) so makekey/start finish quickly - the levels are
     * CPU shares, not fixed priorities, so this takes a bigger slice than
     * alice/bob at LEVEL4 without ever starving them. */
    ca_sem_init(&s_job_sem, "CRYPTO", 0U, 1U);
    ca_os_job("crypto", CA_TH_LEVEL5, task_crypto, NULL, STACK_LARGE, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("alice", CA_TH_LEVEL4, task_alice, NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("bob",   CA_TH_LEVEL4, task_bob,   NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("hb",    CA_TH_LEVEL3, task_hb,    NULL, STACK_SMALL,  CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* EN_AES && EN_RNG && EN_HASH && EN_FLASH && EN_KEYS_STORE &&
        * EN_AES_CHANNEL_SERVICE && EN_KEYSTORE_SERVICE not all enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m47a, "=== Example 47 ===");
PRIVATE_STRING(m47b, "EN_AES / EN_RNG / EN_HASH / EN_FLASH / EN_KEYS_STORE / "
                     "EN_AES_CHANNEL_SERVICE / EN_KEYSTORE_SERVICE disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m47a);
    PUT_N(&priv_m47b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
