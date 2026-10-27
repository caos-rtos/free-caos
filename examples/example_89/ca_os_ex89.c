// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 89 — encrypted files on SD: FAT service + AES channel + keystore
 *
 * Hardware: any board ex58 runs on (SD card on SDIO/SDMMC). Tested target:
 * NUCLEO-H563ZI. AES is in hardware only on H753 (CRYP), software elsewhere.
 *
 * The FAT service writes a file THROUGH the AES channel, synchronously: the
 * plaintext goes into the channel in 512-byte chunks and what comes out is
 * appended to the file; reading runs the pipe the other way. The key is one
 * of the keystore's, chosen by index on every command (`fat ecat <N> <file>`):
 * the channel lives for one call, since a CLI command's heap blocks are freed
 * when its throwaway task ends.
 *
 * CLI (example):
 *   makekey <pwd1> <pwd2> <N>   build a 16-key block, encrypted, in the last
 *                               flash sector (N = SHA-256 evolutions, 1..16)
 *   start <pwd2>                unlock the keystore: keys loaded in keys_store
 *   lock                        lock the keystore
 *   etest                       full unattended check, key 0/1 (FORMATS the card)
 *
 * CLI (FAT service, EN_CLI_FAT):
 *   fat mount | umount | ls | cat | rm | cd | mkdir | ...   the plain commands:
 *                               `fat cat` on an encrypted file shows the raw
 *                               bytes on the card - i.e. that it IS encrypted
 *   fat ewrite <N> <file> <text>  write <text> encrypted with keystore key N
 *   fat ecat <N> <file>           read back decrypted with key N
 *
 * Typical session:
 *   makekey alpha beta 4 / start beta / fat mount
 *   fat ewrite 3 s.txt hello world / fat cat s.txt / fat ecat 3 s.txt
 */

#include "caos.h"

#if EN_FAT && EN_FAT_SERVICE && EN_FAT_CRYPT && EN_SDIO && EN_DMA && EN_RTC && \
    EN_AES && EN_RNG && EN_HASH && EN_FLASH && EN_KEYS_STORE && \
    EN_AES_CHANNEL_SERVICE && EN_KEYSTORE_SERVICE

#include "ff.h"

#define NUM_KEYS   16U

/* Last physical sector, derived at runtime - same choice as ex47, for the
 * same reason: the sector layout differs per family, and a fixed index can
 * land inside the code on a small-page part. */
#define KEY_FLASH_SECTOR  ((u8)(ca_flash_hw_num_sectors() - 1U))

/* ============================================================================
 * Key block: piece = SHA256^N(IV || pad16(pwd))[:16]   (as ex47, no dumps:
 * ex47 prints every intermediate for teaching, here they would only put
 * secrets on the console)
 * ========================================================================== */
static void pad16(const char *pwd, u8 out[16])
{
    u32 len = 0;
    while (pwd[len] && len < 16) len++;
    ca_memcpy(out, pwd, len);
    if (len < 16) ca_memset(out + len, 0, 16 - len);
}

static bool derive_piece(const u8 IV[16], const char *pwd, u32 n_evolve, u8 piece[16])
{
    u8 seed[32];
    ca_memcpy(seed, IV, 16);
    pad16(pwd, seed + 16);

    u8  digest[32];
    u32 dlen = 32;
    bool ok = ca_hash_compute(CA_HASH_ALGO_SHA256, seed, 32, digest, &dlen);
    for (u32 i = 1; ok && i < n_evolve; i++) {
        dlen = 32;
        ok = ca_hash_compute(CA_HASH_ALGO_SHA256, digest, 32, digest, &dlen);
    }
    if (ok) ca_memcpy(piece, digest, 16);
    ca_memzero(seed,   sizeof(seed));
    ca_memzero(digest, sizeof(digest));
    return ok;
}

/* Big buffers static, not on the worker stack (see ex47: on the CLI stack
 * they overflowed into the next task). Only the worker task uses them. */
static ca_flash_key_entry_t s_keys[NUM_KEYS];
static u8 s_plain[NUM_KEYS * sizeof(ca_flash_key_entry_t) + 16];
static u8 s_enc  [NUM_KEYS * sizeof(ca_flash_key_entry_t) + 16];
static u8 s_flash[sizeof(ca_flash_key_block_hdr_t) + NUM_KEYS * sizeof(ca_flash_key_entry_t) + 16 + 32];

static bool do_makekey(const char *pwd1, const char *pwd2, u32 n_evolve)
{
    u8 IV[16], piece1[16], piece2[16];
    if (!ca_rng_fill(IV, 16))                              { ca_stdout_put_n("makekey: RNG fail"); return false; }
    if (!derive_piece(IV, pwd1, n_evolve, piece1))         { ca_stdout_put_n("makekey: piece1 fail"); return false; }
    if (!derive_piece(IV, pwd2, n_evolve, piece2))         { ca_stdout_put_n("makekey: piece2 fail"); return false; }

    for (u32 i = 0; i < NUM_KEYS; i++) {
        s_keys[i].key_id = (u16)i;
        ca_memset(s_keys[i].label, 0, KSTORE_SVC_LABEL_SIZE);
        ca_snprintf((char *)s_keys[i].label, KSTORE_SVC_LABEL_SIZE, "k%02u", (unsigned)i);
        if (!ca_rng_fill(s_keys[i].key, 32)) { ca_stdout_put_n("makekey: RNG key fail"); return false; }
        s_keys[i].start = 0;
        s_keys[i].stop  = 0;
    }

    /* CYPHER_KEY = SHA256^N(IV || piece1 || piece2) */
    u8 kdf_seed[48];
    ca_memcpy(kdf_seed,      IV,     16);
    ca_memcpy(kdf_seed + 16, piece1, 16);
    ca_memcpy(kdf_seed + 32, piece2, 16);
    u8  cypher_key[32];
    u32 dlen = 32;
    bool ok = ca_hash_compute(CA_HASH_ALGO_SHA256, kdf_seed, 48, cypher_key, &dlen);
    for (u32 i = 1; ok && i < n_evolve; i++) {
        dlen = 32;
        ok = ca_hash_compute(CA_HASH_ALGO_SHA256, cypher_key, 32, cypher_key, &dlen);
    }
    ca_memzero(kdf_seed, sizeof(kdf_seed));
    ca_memzero(piece2,   sizeof(piece2));
    if (!ok) { ca_memzero(cypher_key, 32); ca_stdout_put_n("makekey: KDF fail"); return false; }

    u8 gcm_nonce[12];
    if (!ca_rng_fill(gcm_nonce, 12)) { ca_memzero(cypher_key, 32); ca_stdout_put_n("makekey: nonce fail"); return false; }

    u32 plain_len  = NUM_KEYS * sizeof(ca_flash_key_entry_t);
    u32 enc_len    = (plain_len + 15U) & ~15U;
    u32 total_size = sizeof(ca_flash_key_block_hdr_t) + enc_len + KSTORE_SVC_TAG_SIZE;

    ca_flash_key_block_hdr_t hdr;
    hdr.size     = total_size;
    ca_memcpy(hdr.IV, IV, 16);
    hdr.n_evolve = n_evolve;
    ca_memcpy(hdr.p1, piece1, 16);
    hdr.n_key    = (u16)NUM_KEYS;
    ca_memcpy(hdr.gcm_nonce, gcm_nonce, 12);

    ca_memset(s_plain, 0, sizeof(s_plain));
    ca_memcpy(s_plain, s_keys, plain_len);
    ca_memzero(s_keys, sizeof(s_keys));

    u8 tag[16];
    ca_aes_ctx_t ctx;
    ok = ca_aes_ctx_setup(&ctx, CA_AES_MODE_GCM, CA_AES_KEY_256, cypher_key, gcm_nonce) &&
         ca_aes_gcm_encrypt(&ctx, (const u8 *)&hdr, sizeof(hdr), s_plain, s_enc, enc_len, tag);
    ca_memzero(cypher_key, sizeof(cypher_key));
    ca_memzero(s_plain,    sizeof(s_plain));
    ca_memzero(&ctx,       sizeof(ctx));
    if (!ok) { ca_stdout_put_n("makekey: AES-GCM fail"); return false; }

    u32 flash_addr = ca_flash_get_sector_addr(KEY_FLASH_SECTOR);
    if (!flash_addr || total_size > ca_flash_get_sector_size(KEY_FLASH_SECTOR)) {
        ca_stdout_put_n("makekey: invalid flash sector"); return false;
    }

    u32 write_len = (total_size + 31U) & ~31U;
    ca_memset(s_flash, 0xFF, sizeof(s_flash));
    ca_memcpy(s_flash,                          &hdr,  sizeof(hdr));
    ca_memcpy(s_flash + sizeof(hdr),            s_enc, enc_len);
    ca_memcpy(s_flash + sizeof(hdr) + enc_len,  tag,   16);

    if (!ca_flash_erase_sector_num(KEY_FLASH_SECTOR)) { ca_stdout_put_n("makekey: erase fail"); return false; }
    if (!ca_flash_hw_unlock())                        { ca_stdout_put_n("makekey: unlock fail"); return false; }
    bool wr_ok = ca_flash_hw_write(flash_addr, s_flash, write_len);
    ca_flash_hw_lock();
    if (!wr_ok) { ca_stdout_put_n("makekey: write fail"); return false; }

    if (ca_keystore_service_init(flash_addr) != 0) {
        ca_stdout_put_n("makekey: keystore_service_init fail"); return false;
    }
    ca_stdout_printf_n("makekey: OK - %u keys, %u bytes @ 0x%08X",
                       (unsigned)NUM_KEYS, (unsigned)total_size, (unsigned)flash_addr);
    return true;
}

/* Unlock only: the key index is given on each fat ewrite/ecat. */
static bool do_start(const char *pwd2)
{
    u32 flash_addr = ca_flash_get_sector_addr(KEY_FLASH_SECTOR);
    if (!ca_keystore_service_is_active() && ca_keystore_service_init(flash_addr) != 0) {
        ca_stdout_put_n("start: no key block in flash - run makekey first");
        return false;
    }

    ca_flash_key_block_hdr_t hdr;
    ca_memcpy(&hdr, (const void *)flash_addr, sizeof(hdr));

    u8 piece2[16];
    if (!derive_piece(hdr.IV, pwd2, hdr.n_evolve, piece2)) {
        ca_stdout_put_n("start: piece2 derive fail"); return false;
    }
    i32 r = ca_keystore_service_unlock(piece2);
    ca_memzero(piece2, sizeof(piece2));
    if (r != 0) { ca_stdout_put_n("start: unlock fail (wrong password?)"); return false; }

    ca_stdout_put_n("start: OK - keystore unlocked, keys 0..15 usable by 'fat ewrite/ecat <N>'");
    return true;
}

/* ============================================================================
 * etest — full unattended check
 * --------------------------------------------------------------------------
 * Needs the keystore unlocked (`start`). DESTRUCTIVE: it partitions and
 * formats the card first, so it runs on any card in the slot. Every step must
 * hold for PASS; the negative ones (wrong key, modified byte) PASS when the
 * read is REFUSED.
 * ========================================================================== */
/* <uptime ms>_e89.bin: a new file on every run, sorted by time, and LEFT on
 * the card for a manual `fat cat` (ciphertext) / `fat ecat` (plaintext). */
static char s_et_file[32];
#define ET_FILE   s_et_file
#define ET_LEN    2000U        /* 3 full packets + 1 short: exercises the tail */

static u8  s_et_plain[ET_LEN];
static u8  s_et_raw[64];
static u32 s_et_pos;
static bool s_et_match;

static bool et_compare(const u8 *data, u32 len, void *user)
{
    (void)user;
    if (s_et_pos + len > ET_LEN || ca_memcmp(data, s_et_plain + s_et_pos, len) != 0)
        s_et_match = false;
    s_et_pos += len;
    return true;
}

static bool et_read_ok(u32 key)
{
    s_et_pos = 0U; s_et_match = true;
    u32 total = 0U;
    bool ok = ca_fat_read_enc(key, ET_FILE, et_compare, NULL, &total);
    return ok && s_et_match && total == ET_LEN;
}

/* On-disk size the channel must produce for ET_LEN bytes in 512-byte chunks. */
static u32 et_expected_size(void)
{
    u32 size = 0U, left = ET_LEN;
    while (left) {
        u32 n = left > FAT_CRYPT_CHUNK ? FAT_CRYPT_CHUNK : left;
        size += AES_CH_OVERHEAD + ((n + 15U) & ~15U);
        left -= n;
    }
    return size;
}

/* Raw look at the file: right size, and the first bytes are NOT the plaintext. */
static bool et_raw_is_encrypted(void)
{
    FIL f;
    UINT br = 0;
    ca_fat_lock();
    bool ok = (f_open(&f, ET_FILE, FA_READ) == FR_OK);
    u32 size = ok ? (u32)f_size(&f) : 0U;
    if (ok) { ok = (f_read(&f, s_et_raw, sizeof(s_et_raw), &br) == FR_OK); f_close(&f); }
    ca_fat_unlock();
    if (!ok) { ca_stdout_put_n("[etest] raw open/read failed"); return false; }

    ca_stdout_printf_n("[etest] on disk %u bytes (expected %u), plain %u", (unsigned)size,
                       (unsigned)et_expected_size(), (unsigned)ET_LEN);
    ca_hexdump(s_et_raw, 32);
    /* Bytes 14..45 are the first ciphertext block of packet 0: must differ. */
    return size == et_expected_size() &&
           ca_memcmp(s_et_raw + AES_CH_IV_SIZE + 2U, s_et_plain, 32) != 0;
}

/* Flip one bit inside the ciphertext of packet 1. */
static bool et_tamper(void)
{
    FIL f;
    UINT n = 0;
    u8 b = 0;
    const u32 off = (AES_CH_OVERHEAD + FAT_CRYPT_CHUNK) + 100U;
    ca_fat_lock();
    bool ok = (f_open(&f, ET_FILE, FA_READ | FA_WRITE) == FR_OK);
    if (ok) {
        ok = (f_lseek(&f, off) == FR_OK) && (f_read(&f, &b, 1, &n) == FR_OK) && n == 1U;
        b ^= 0x01U;
        ok = ok && (f_lseek(&f, off) == FR_OK) && (f_write(&f, &b, 1, &n) == FR_OK) && n == 1U;
        f_close(&f);
    }
    ca_fat_unlock();
    ca_stdout_printf_n("[etest] flipped bit 0 of byte %u (packet 1)", (unsigned)off);
    return ok;
}

#define ET_STEP(label, cond)                                                   \
    do {                                                                       \
        steps++;                                                               \
        ca_stdout_printf_n("[etest] %2u. %s", (unsigned)steps, (label));       \
        if (!(cond)) {                                                         \
            failed++;                                                          \
            ca_stdout_printf_n("[etest]     ^^ FAILED: %s", (label));          \
        }                                                                      \
    } while (0)

static void do_etest(void)
{
    u32 steps = 0U, failed = 0U;
    u32 t0 = ca_os_tick_ms();
    for (u32 i = 0; i < ET_LEN; i++) s_et_plain[i] = (u8)(i * 7U + (i >> 8));
    ca_snprintf(s_et_file, sizeof(s_et_file), "%u_e89.bin", (unsigned)(u32)ca_os_tick_64_ms());

    ca_stdout_put_n("========================================================");
    ca_stdout_put_n("[etest] EX89 encrypted files (DESTRUCTIVE: formats the card)");
    ca_stdout_put_n("========================================================");

    ET_STEP("keystore unlocked",                ca_keystore_service_is_active());
    /* The card is formatted here, not assumed: whatever is in the slot. */
    ET_STEP("fdisk 64     (partition table)",   ca_fat_fdisk(64U));
    ET_STEP("mkfs         (format FAT32)",      ca_fat_mkfs());
    ET_STEP("mount",                            ca_fat_mount());
    ET_STEP("ewrite key 0 (2000 bytes)",        ca_fat_write_enc(0U, ET_FILE, s_et_plain, ET_LEN));
    ET_STEP("raw bytes are ciphertext",         et_raw_is_encrypted());
    ET_STEP("eread key 0 == plaintext",         et_read_ok(0U));
    ET_STEP("eread key 1 REFUSED",              !et_read_ok(1U));
    ET_STEP("eread key 0 again",                et_read_ok(0U));
    ET_STEP("flip one ciphertext bit",          et_tamper());
    ET_STEP("eread of modified file REFUSED",   !et_read_ok(0U));
    /* Put the bit back: the file stays on the card for manual inspection,
     * and it must be the intact one - readable with key 0. */
    ET_STEP("flip the bit back",                et_tamper());
    ET_STEP("eread restored file == plaintext", et_read_ok(0U));

    u32 ms = ca_os_tick_ms() - t0;
    ca_stdout_put_n("========================================================");
    ca_stdout_printf_n("[etest] file left on the card: 'fat cat %s' / 'fat ecat 0 %s'",
                       s_et_file, s_et_file);
    if (failed == 0U)
        ca_stdout_printf_n("[etest] RESULT: PASS  (%u/%u steps, %u ms)",
                           (unsigned)steps, (unsigned)steps, (unsigned)ms);
    else
        ca_stdout_printf_n("[etest] RESULT: FAIL  (%u/%u steps failed, %u ms)",
                           (unsigned)failed, (unsigned)steps, (unsigned)ms);
    ca_stdout_put_n("========================================================");
}

/* ============================================================================
 * Worker task: makekey/start/etest are long (software SHA-256 chain, AES on
 * 2 KB) and deep - they run here on STACK_LARGE, not in the CLI task (2 KB).
 * ========================================================================== */
typedef enum { JOB_NONE = 0, JOB_MAKEKEY, JOB_START, JOB_ETEST } job_t;

static volatile job_t s_job = JOB_NONE;
static char           s_pwd1[32];
static char           s_pwd2[32];
static u32            s_n_evolve;
static ca_semaphore_t    s_job_sem;

static u32 task_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ca_sem_wait(&s_job_sem);
        switch (s_job) {
        case JOB_MAKEKEY: do_makekey(s_pwd1, s_pwd2, s_n_evolve); break;
        case JOB_START:   do_start(s_pwd2);                       break;
        case JOB_ETEST:   do_etest();                             break;
        default:                                                  break;
        }
        ca_memzero(s_pwd1, sizeof(s_pwd1));
        ca_memzero(s_pwd2, sizeof(s_pwd2));
        s_job = JOB_NONE;
    }
    return 0;
}

static void post(job_t job)
{
    if (s_job != JOB_NONE) { CLI_PUT_N("ex89: busy, retry in a moment"); return; }
    s_job = job;
    ca_sem_post(&s_job_sem);
}

/* ============================================================================
 * CLI
 * ========================================================================== */
static const char *word(const char *s, char *out, u32 cap)
{
    u32 i = 0;
    s = ca_skip_sp(s);
    while (*s != '\0' && *s != ' ' && i + 1U < cap) out[i++] = *s++;
    out[i] = '\0';
    return ca_skip_sp(s);
}

static bool ex89_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("makekey   : <pwd1> <pwd2> <N> build the 16-key block in flash");
        CLI_PUT_N("start     : <pwd2> unlock the keystore, then 'fat ecat <N> <file>'");
        CLI_PUT_N("lock      : lock the keystore");
        CLI_PUT_N("etest     : encrypted-file check, keys 0/1 (needs start; FORMATS the card)");
        return false;
    }

    if (ca_strncmp(p, "makekey", 7) == 0) {
        char n_s[8];
        const char *a = word(p + 7, s_pwd1, sizeof(s_pwd1));
        a = word(a, s_pwd2, sizeof(s_pwd2));
        word(a, n_s, sizeof(n_s));
        u16 idx = 0;
        u32 n = n_s[0] ? (u32)ca_strtol_index(n_s, 10, &idx) : 0U;
        if (!s_pwd1[0] || !s_pwd2[0] || n < 1U || n > 16U) {
            ca_memzero(s_pwd1, sizeof(s_pwd1)); ca_memzero(s_pwd2, sizeof(s_pwd2));
            CLI_PUT_N("makekey: usage: makekey <pwd1> <pwd2> <N 1..16>");
            return true;
        }
        s_n_evolve = n;
        post(JOB_MAKEKEY);
        return true;
    }

    if (ca_strncmp(p, "start", 5) == 0) {
        word(p + 5, s_pwd2, sizeof(s_pwd2));
        if (!s_pwd2[0]) { CLI_PUT_N("start: usage: start <pwd2>"); return true; }
        post(JOB_START);
        return true;
    }

    if (ca_strncmp(p, "lock", 4) == 0 && (p[4] == '\0' || p[4] == ' ')) {
        ca_keystore_service_lock();
        CLI_PUT_N("lock: keystore locked");
        return true;
    }

    if (ca_strncmp(p, "etest", 5) == 0) { post(JOB_ETEST); return true; }

    return false;
}

static ca_cli_node_t s_ex89_cli = { ex89_cli_handler, NULL };

/* ============================================================================
 * Heartbeat
 * ========================================================================== */
static u32 task_heartbeat(void *arg)
{
    (void)arg;
    ca_led_init();
    for (;;) { ca_led_toggle(1U); ca_os_sleep(500); }
    return 0;
}

/* ============================================================================
 * Configure / main
 * ========================================================================== */
void ca_os_configure(void)
{
    EVAL_DMA( ca_dma_init(); )          /* before SDIO */
    EVAL_SDIO( (void)ca_sdio_init(); )  /* absent card is a normal state */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
    )
    ca_fat_service_init();              /* registers `fat` (needs ca_cli_init) */
    EVAL_CLI(
        ca_cli_register(&s_ex89_cli);
        ca_cli_create_thread();
    )

    ca_flash_init();
    ca_flash_irq_init();
    if (!ca_aes_init())  CA_SAFE_FAIL("ca_aes_init");
    if (!ca_hash_init()) CA_SAFE_FAIL("ca_hash_init");
    if (!ca_rng_init())  CA_SAFE_FAIL("ca_rng_init");
    if (ca_keys_store_init(MAX_SECURE_KEYS) != 0) CA_SAFE_FAIL("ca_keys_store_init");
}

void ca_os_main(void)
{
    ca_sem_init(&s_job_sem, "EX89", 0U, 1U);
    ca_os_job("worker",    CA_TH_LEVEL5, task_worker,    NULL, STACK_LARGE, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL3, task_heartbeat, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* flags not enabled */

PRIVATE_STRING(m89a, "=== Example 89 ===");
PRIVATE_STRING(m89b, "EN_FAT_SERVICE / EN_FAT_CRYPT / EN_SDIO / EN_AES / EN_KEYSTORE_SERVICE ... disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m89a);
    PUT_N(&priv_m89b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
