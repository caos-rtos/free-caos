// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 143 — AES-GCM split into its phases, timed with the DWT cycle counter
 *
 * Where does the time of an AES-GCM encryption go? The public API is enough
 * to take it apart, without touching aes.c:
 *
 *   setup      ca_aes_ctx_setup() + the first     key schedule and, in software,
 *              ca_aes_gcm_encrypt(aad 0, data 0)  H and its GHASH table (built
 *                                                 lazily on the first call)
 *   gcm_base   ca_aes_gcm_encrypt(aad 0, data 0)  fixed cost of every further
 *              on a ready context                 call: J0, length block, tag
 *   aes_ecb    ca_aes_encrypt() in ECB, N bytes   the AES rounds alone (CTR in
 *                                                 GCM is the same rounds on a
 *                                                 counter, plus an XOR)
 *   ghash      ca_aes_gcm_encrypt(aad N, data 0)  GHASH alone over N bytes
 *              minus gcm_base
 *   gcm_full   ca_aes_gcm_encrypt(aad 0, data N)  the whole thing
 *              minus gcm_base
 *
 * and gcm_full should come out close to aes_ecb + ghash: the remainder is the
 * counter, the XOR and the calls. The split says which part to optimise first
 * (TODO "AES software: DA OTTIMIZZARE", 20/11/2026: GCM 105-226 KB/s on the
 * parts without CRYP; GHASH is the suspect, measured here instead of guessed).
 *
 * On parts with an AES unit (F439, H753, H773) the same calls go to the
 * hardware: the numbers are then a reference, not the software path.
 *
 * Method: DWT->CYCCNT (enabled and, on the M7, unlocked by caOS at init).
 * IRQs stay ON: a 4 KB software GCM lasts milliseconds, too long to mask. Each
 * figure is the MINIMUM of E143_REPS runs, taken after a pause that lets stdout
 * drain - an interrupt can only add cycles, so the minimum is the clean run.
 * The IV is reused across the runs: fine for a timing, never for real data.
 *
 * CLI:
 *   e143 run [128|256]   table of the phases, 16 B .. 4 KB (default: both keys)
 *   e143 test            one pass of every phase + the GCM known-answer test,
 *                        "E143 RESULT PASS|FAIL" for the bench
 *   e143 sha             SHA-256 and SHA-512 one-shot, 64 B .. 4 KB, same method
 *                        (software on every part: TLS, SSH, PBKDF2 use them)
 */

#include "caos.h"

#if EN_AES && ENABLE_CLI

#define E143_MAX       4096U
#define E143_REPS      5U
#define E143_PAUSE_MS  30U

static u8 s_in[E143_MAX]  __attribute__((aligned(4)));
static u8 s_out[E143_MAX] __attribute__((aligned(4)));

static const u32 s_sizes[] = { 16U, 64U, 256U, 1024U, 4096U };

static const u8 s_key[32] = {
    0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,
    0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4
};
static const u8 s_iv[16] = {
    0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88,0x00,0x00,0x00,0x01
};

typedef enum { PH_SETUP = 0, PH_GCM_BASE, PH_AES_ECB, PH_GHASH_AAD, PH_GCM_FULL } e143_phase_t;

/* One run of a phase; false if the API refused */
static bool run_phase(e143_phase_t ph, ca_aes_keysize_t ks, u32 n, u32 *cycles)
{
    ca_aes_ctx_t ctx;
    u8 tag[AES_GCM_TAG_SIZE];
    bool ok = true;
    const ca_aes_mode_t mode = (ph == PH_AES_ECB) ? CA_AES_MODE_ECB : CA_AES_MODE_GCM;

    if (ph != PH_SETUP) {
        /* a ready context: the software schedule (and GHASH table) is built
         * lazily by the first call, which would otherwise land in the count */
        if (!ca_aes_ctx_setup(&ctx, mode, ks, s_key, s_iv)) return false;
        const bool warm = (mode == CA_AES_MODE_ECB)
                        ? ca_aes_encrypt(&ctx, s_in, s_out, 16U)
                        : ca_aes_gcm_encrypt(&ctx, NULL, 0U, NULL, NULL, 0U, tag);
        if (!warm) return false;
    }

    const u32 t0 = DWT->CYCCNT;
    switch (ph) {
    case PH_SETUP:     ok = ca_aes_ctx_setup(&ctx, CA_AES_MODE_GCM, ks, s_key, s_iv) &&
                            ca_aes_gcm_encrypt(&ctx, NULL, 0U, NULL, NULL, 0U, tag);           break;
    case PH_GCM_BASE:  ok = ca_aes_gcm_encrypt(&ctx, NULL, 0U, NULL, NULL, 0U, tag);           break;
    case PH_AES_ECB:   ok = ca_aes_encrypt(&ctx, s_in, s_out, n);                              break;
    case PH_GHASH_AAD: ok = ca_aes_gcm_encrypt(&ctx, s_in, n, NULL, NULL, 0U, tag);            break;
    case PH_GCM_FULL:  ok = ca_aes_gcm_encrypt(&ctx, NULL, 0U, s_in, s_out, n, tag);           break;
    }
    *cycles = DWT->CYCCNT - t0;
    ca_memzero(&ctx, sizeof ctx);
    return ok;
}

/* Minimum over E143_REPS runs, after a pause for stdout; 0 if refused */
static u32 measure(e143_phase_t ph, ca_aes_keysize_t ks, u32 n)
{
    u32 best = 0xFFFFFFFFU;
    ca_os_sleep(E143_PAUSE_MS);
    for (u32 r = 0U; r < E143_REPS; r++) {
        u32 c;
        if (!run_phase(ph, ks, n, &c)) return 0U;
        if (c < best) best = c;
    }
    return best;
}

/* cycles/byte x10, and KB/s at the core clock */
static u32 cpb10(u32 cyc, u32 n)  { return n ? (u32)(((u64)cyc * 10U) / n) : 0U; }
static u32 kbps(u32 cyc, u32 n)
{
    return cyc ? (u32)(((u64)n * (u64)SYSCLK_FREQ_HZ) / ((u64)cyc * 1024U)) : 0U;
}

static bool run_table(ca_aes_keysize_t ks)
{
    const u32 bits = (ks == CA_AES_KEY_256) ? 256U : 128U;
    bool ok = true;

    const u32 setup = measure(PH_SETUP, ks, 0U);
    const u32 base  = measure(PH_GCM_BASE, ks, 0U);
    CLI_PRINTF_N("E143 AES-%u %s, core %u MHz: setup %u cyc, gcm_base %u cyc",
                 bits, ca_aes_is_hw() ? "HW" : "SW", (u32)(SYSCLK_FREQ_HZ / 1000000U), setup, base);
    CLI_PUT_N("E143  bytes |   aes_ecb c/B   KB/s |    ghash c/B   KB/s |  gcm_full c/B   KB/s | aes% ghash% rest%");
    if (setup == 0U || base == 0U) ok = false;

    for (u32 i = 0U; i < sizeof(s_sizes) / sizeof(s_sizes[0]); i++) {
        const u32 n   = s_sizes[i];
        const u32 aes = measure(PH_AES_ECB, ks, n);
        const u32 aad = measure(PH_GHASH_AAD, ks, n);
        const u32 ful = measure(PH_GCM_FULL, ks, n);
        if (aes == 0U || aad == 0U || ful == 0U) { ok = false; continue; }
        const u32 gh  = (aad > base) ? aad - base : 0U;
        const u32 gc  = (ful > base) ? ful - base : 0U;
        const u32 pa  = gc ? (u32)(((u64)aes * 100U) / gc) : 0U;
        const u32 pg  = gc ? (u32)(((u64)gh * 100U) / gc) : 0U;
        const i32 pr  = 100 - (i32)pa - (i32)pg;
        CLI_PRINTF_N("E143 %6u | %8u.%u %6u | %8u.%u %6u | %8u.%u %6u | %4u %6u %5d",
                     n,
                     cpb10(aes, n) / 10U, cpb10(aes, n) % 10U, kbps(aes, n),
                     cpb10(gh, n) / 10U,  cpb10(gh, n) % 10U,  kbps(gh, n),
                     cpb10(gc, n) / 10U,  cpb10(gc, n) % 10U,  kbps(gc, n),
                     pa, pg, (int)pr);
    }
    return ok;
}

/* Bench verdict: every phase answers, the known-answer test passes, and the
 * software split adds up (aes + ghash within 70..130% of gcm_full at 1 KB) */
static void run_test(void)
{
    bool ok = true;
#if EN_AES_GCM_SW_TEST
    if (ca_aes_gcm_selftest() != 0U) { CLI_PUT_N("E143 FAIL gcm known-answer test"); ok = false; }   /* 0 = all passed */
#endif
    const ca_aes_keysize_t ks[2] = { CA_AES_KEY_128, CA_AES_KEY_256 };
    for (u32 k = 0U; k < 2U; k++) {
        const u32 base = measure(PH_GCM_BASE, ks[k], 0U);
        const u32 aes  = measure(PH_AES_ECB, ks[k], 1024U);
        const u32 aad  = measure(PH_GHASH_AAD, ks[k], 1024U);
        const u32 ful  = measure(PH_GCM_FULL, ks[k], 1024U);
        const u32 bits = (k == 0U) ? 128U : 256U;
        if (base == 0U || aes == 0U || aad == 0U || ful == 0U) {
            CLI_PRINTF_N("E143 FAIL AES-%u: a phase was refused by the API", bits);
            ok = false;
            continue;
        }
        const u32 gc  = (ful > base) ? ful - base : 1U;
        const u32 sum = aes + ((aad > base) ? aad - base : 0U);
        const u32 pct = (u32)(((u64)sum * 100U) / gc);
        CLI_PRINTF_N("E143 AES-%u 1 KB: aes %u + ghash %u = %u%% of gcm_full %u cyc",
                     bits, aes, sum - aes, pct, gc);
        /* Software only: the AES unit runs CTR and GHASH in parallel, so on
         * the hardware the two phases overlap and their sum is ~180% */
        if (!ca_aes_is_hw() && (pct < 70U || pct > 130U)) {
            CLI_PUT_N("E143 FAIL the split does not add up");
            ok = false;
        }
    }
    CLI_PRINTF_N("E143 RESULT %s", ok ? "PASS" : "FAIL");
}

#if EN_SHA256 || EN_SHA512
static void run_sha(void)
{
    static const u32 sz[] = { 64U, 256U, 1024U, 4096U };
    u8 dg[64];
    CLI_PRINTF_N("E143 SHA software, core %u MHz", (u32)(SYSCLK_FREQ_HZ / 1000000U));
    CLI_PUT_N("E143  bytes |  sha256 c/B   KB/s |  sha512 c/B   KB/s");
    for (u32 i = 0U; i < sizeof(sz) / sizeof(sz[0]); i++) {
        u32 b256 = 0xFFFFFFFFU, b512 = 0xFFFFFFFFU;
        ca_os_sleep(E143_PAUSE_MS);
        for (u32 r = 0U; r < E143_REPS; r++) {
#if EN_SHA256
            ca_sha256_ctx_t c2;
            u32 t0 = DWT->CYCCNT;
            ca_sha256(&c2, s_in, sz[i], dg);
            u32 c = DWT->CYCCNT - t0;
            if (c < b256) b256 = c;
#endif
#if EN_SHA512
            ca_sha512_ctx_t c5;
            u32 t1 = DWT->CYCCNT;
            ca_sha512(&c5, s_in, sz[i], dg);
            u32 d = DWT->CYCCNT - t1;
            if (d < b512) b512 = d;
#endif
        }
        if (b256 == 0xFFFFFFFFU) b256 = 0U;
        if (b512 == 0xFFFFFFFFU) b512 = 0U;
        CLI_PRINTF_N("E143 %6u | %7u.%u %6u | %7u.%u %6u", sz[i],
                     cpb10(b256, sz[i]) / 10U, cpb10(b256, sz[i]) % 10U, kbps(b256, sz[i]),
                     cpb10(b512, sz[i]) / 10U, cpb10(b512, sz[i]) % 10U, kbps(b512, sz[i]));
    }
    CLI_PUT_N("E143 sha done");
}
#endif

/* e143 — the line arrives without the command word */
static bool e143_cmd(c_str *args)
{
    const char *p = (*args)->data;
    if (ca_strncmp(p, "run", 3) == 0) {
        p = ca_skip_sp(p + 3);
        bool ok = true;
        if (ca_strncmp(p, "256", 3) != 0) ok &= run_table(CA_AES_KEY_128);
        if (ca_strncmp(p, "128", 3) != 0) ok &= run_table(CA_AES_KEY_256);
        CLI_PRINTF_N("E143 run %s", ok ? "done" : "FAILED (a phase was refused)");
    } else if (ca_strncmp(p, "test", 4) == 0) {
        run_test();
#if EN_SHA256 || EN_SHA512
    } else if (ca_strncmp(p, "sha", 3) == 0) {
        run_sha();
#endif
    } else {
        CLI_PUT_N("e143      : run [128|256] | test   - AES-GCM phases timed with DWT");
    }
    return true;
}

static const ca_cli_cmd_t s_e143_cmds[] = {
    { "e143", "e143      : run [128|256] | test | sha   - AES-GCM phases, SHA, timed with DWT", { e143_cmd, STACK_LARGE } },
};

static ca_cli_table_t s_e143_table = {
    "e143", s_e143_cmds, (u16)(sizeof(s_e143_cmds) / sizeof(s_e143_cmds[0])), NULL
};

void ca_os_configure(void)
{
    for (u32 i = 0U; i < E143_MAX; i++) s_in[i] = (u8)(i * 13U + 7U);
    EVAL_AES(ca_aes_init();)
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
}

void ca_os_main(void)
{
    ca_cli_register_table(&s_e143_table);
    ca_os_run(true);
}

#else  /* EN_AES && ENABLE_CLI */

PRIVATE_STRING(m143a, "=== Example 143: AES-GCM phases ===");
PRIVATE_STRING(m143b, "EN_AES / ENABLE_CLI ==0, example disabled");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m143a);
    PUT_N(&priv_m143b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
