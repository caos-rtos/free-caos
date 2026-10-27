// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex104.c
 * @brief          : Example 104 — SPDIFRX fed by the SAI in S/PDIF mode
 *                   (F446, F767, H753)
 ******************************************************************************
 * @details
 * The SAI block A, in SPDIF mode, transmits one IEC 60958 block (192 frames,
 * 384 subframes) over and over by circular DMA; the SPDIFRX receives it. One
 * jumper on the same connector of the three Nucleo:
 *
 *     PE6 SD_A  CN9-20  ---->  PD7 SPDIFRX_IN  CN9-2
 *
 *   spdif test [fs]    one rate, four checks:
 *                        sync     the rate the SPDIFRX measures is the one
 *                                 ca_sai_init() declared (2%)
 *                        cs       the 24 channel status bytes, sent in the C
 *                                 bits of channel A, come back from SPDIFRX_CSR
 *                        dma      4 blocks by DMA, polled 2 blocks: every word
 *                        poll     equal to the one sent - sample, V, U, C,
 *                                 no parity error, preamble B/M/W where due
 *                      and no error flag on the way
 *   spdif all          32 / 44.1 / 48 / 96 kHz, one verdict:
 *                      "SPDIF ALL PASS" / "SPDIF ALL FAIL"; a rate beyond
 *                      SPDIFRX_CLK / 704 is skipped (H753: 96 kHz)
 *   spdif info         clocks and the last error flags
 *   wire               the jumper as plain GPIO, then reboot
 *
 * Both ends start at a block: the SAI begins its stream with a B preamble on
 * word 0, the SPDIFRX delivers from the first B after synchronizing, so
 * received word k is sent word k mod 384 - no lag to search.
 ******************************************************************************
 */

#include "caos.h"

#if EN_SPDIFRX && EN_SAI && EN_DMA

#define EX104_BLOCK     384U          /* subframes in an IEC 60958 block */
#define EX104_DMA_WORDS (4U * EX104_BLOCK)
#define EX104_POLL_WORDS (2U * EX104_BLOCK)
#define EX104_FS_TOL_PM 20U           /* measured fs within 2% (H7: 1% resolution) */

#define WIRE_TXT        "PE6 SD_A CN9-20 ---- PD7 SPDIFRX_IN CN9-2"

/* Static, not on the stack: on H7 the stacks are in DTCM, out of DMA reach */
static i32 s_tx[EX104_BLOCK];
static u32 s_rx[EX104_DMA_WORDS];
static u8  s_cs[CA_SPDIFRX_CS_BYTES];
static u8  s_cs_rx[CA_SPDIFRX_CS_BYTES];

static u32 s_fs_req = 48000U;
static u32 s_clk;               /* SPDIFRX_CLK of the last run */

/* One block: random 24-bit samples, V on every fifth word, U from a hash,
 * C of both channels from the channel status bytes (bit j = frame j) */
static void ex104_fill(void)
{
    for (u32 i = 0U; i < CA_SPDIFRX_CS_BYTES; i++)
        s_cs[i] = (i == 0U) ? 0x04U : (u8)(i * 11U + 3U);   /* consumer, copy permitted */

    u32 r = 0x2545F491U;
    for (u32 k = 0U; k < EX104_BLOCK; k++) {
        r = r * 1664525U + 1013904223U;
        const u32 j = k / 2U;                               /* frame */
        u32 w = r >> 8;
        if ((k % 5U) == 0U)                          w |= CA_SAI_SPDIF_V;
        if (((k * 0x9E3779B9U) >> 31) != 0U)         w |= CA_SAI_SPDIF_U;
        if ((s_cs[j / 8U] >> (j % 8U)) & 1U)         w |= CA_SAI_SPDIF_C;
        s_tx[k] = (i32)w;
    }
}

/* The word the SPDIFRX gives for sent word k: sample, then V U C one bit
 * higher (PE in between, 0), and the preamble */
static u32 ex104_expect(u32 k)
{
    const u32 t  = (u32)s_tx[k % EX104_BLOCK];
    const u32 pt = ((k % EX104_BLOCK) == 0U) ? CA_SPDIFRX_PT_B
                 : ((k & 1U) == 0U) ? CA_SPDIFRX_PT_M : CA_SPDIFRX_PT_W;
    return (t & 0xFFFFFFU) | ((t & 0x7000000U) << 1) | (pt << 28);
}

static u32 ex104_check(u32 words, u32 *first)
{
    u32 bad = 0U;
    *first = 0xFFFFFFFFU;
    for (u32 k = 0U; k < words; k++)
        if (s_rx[k] != ex104_expect(k)) {
            if (bad == 0U) *first = k;
            bad++;
        }
    return bad;
}

static bool ex104_fs_ok(u32 a, u32 fs)
{
    const u32 d = (a > fs) ? a - fs : fs - a;
    return (u64)d * 1000U <= (u64)fs * EX104_FS_TOL_PM;
}

typedef struct {
    u32 fs, clk, meas;
    u32 cs_n, bad_d, first_d, bad_p, first_p, got_p;
    bool skip, ok_f, ok_c, ok_d, ok_p;
    u32 err;
} ex104_run_t;

static bool ex104_run(ex104_run_t *r)
{
    const ca_sai_config_t cfg = { s_fs_req, 24U, CA_SAI_SPDIF };
    r->fs = ca_sai_init(&cfg);
    if (r->fs == 0U) return false;
    /* After the SAI: on F4/F7 its init re-tunes the PLLI2S both use */
    r->clk = ca_spdifrx_init();
    s_clk  = r->clk;
    if (r->clk == 0U) return false;
    if ((u64)r->fs * 704U > r->clk) { r->skip = true; return true; }

    ex104_fill();
    if (!ca_sai_stream_start(s_tx, NULL, EX104_BLOCK)) return false;
    ca_os_sleep(5U);

    r->meas = ca_spdifrx_sync(100U);
    r->err |= ca_spdifrx_errors();
    r->ok_f = ex104_fs_ok(r->meas, r->fs);

    for (u32 i = 0U; i < CA_SPDIFRX_CS_BYTES; i++) s_cs_rx[i] = 0U;
    r->cs_n = ca_spdifrx_channel_status(s_cs_rx, 200U);
    r->err |= ca_spdifrx_errors();
    r->ok_c = (r->cs_n == CA_SPDIFRX_CS_BYTES);
    for (u32 i = 0U; i < CA_SPDIFRX_CS_BYTES && r->ok_c; i++)
        r->ok_c = (s_cs_rx[i] == s_cs[i]);

    for (u32 k = 0U; k < EX104_DMA_WORDS; k++) s_rx[k] = 0U;
    r->ok_d = ca_spdifrx_read_dma(s_rx, EX104_DMA_WORDS, 500U);
    r->err |= ca_spdifrx_errors();
    r->bad_d = ex104_check(EX104_DMA_WORDS, &r->first_d);
    r->ok_d = r->ok_d && r->bad_d == 0U;

    /* Polled: the data register is two words deep (21 us at 48 kHz), so the
     * console must have sent the echo and the previous line first */
    for (u32 k = 0U; k < EX104_POLL_WORDS; k++) s_rx[k] = 0U;
    ca_os_sleep(30U);
    r->got_p = ca_spdifrx_read(s_rx, EX104_POLL_WORDS, 500U);
    r->err |= ca_spdifrx_errors();
    r->bad_p = ex104_check(EX104_POLL_WORDS, &r->first_p);
    r->ok_p = (r->got_p == EX104_POLL_WORDS) && r->bad_p == 0U;

    ca_sai_stop();
    return r->ok_f && r->ok_c && r->ok_d && r->ok_p && r->err == 0U;
}

static void ex104_print(const ex104_run_t *r, bool ok)
{
    if (r->skip) {
        CLI_PRINTF_N("SPDIF %6u -> %6u Hz  skip: SPDIFRX_CLK %u Hz < 704 fs",
                     s_fs_req, r->fs, r->clk);
        return;
    }
    CLI_PRINTF_N("SPDIF %6u -> %6u Hz (meas %6u)  cs %s/%u  dma %s  poll %s/%u  err %02X  %s",
                 s_fs_req, r->fs, r->meas, r->ok_c ? "ok" : "BAD", r->cs_n,
                 r->ok_d ? "ok" : "BAD", r->ok_p ? "ok" : "BAD", r->got_p,
                 r->err, ok ? "PASS" : "FAIL");
    if (!r->ok_d && r->first_d != 0xFFFFFFFFU)
        CLI_PRINTF_N("SPDIF   dma: %u words differ, first #%u got %08X want %08X",
                     r->bad_d, r->first_d, s_rx[r->first_d], ex104_expect(r->first_d));
    if (!r->ok_p && r->bad_p != 0U)
        CLI_PRINTF_N("SPDIF   poll: %u words differ, first #%u got %08X %08X %08X want %08X %08X %08X",
                     r->bad_p, r->first_p, s_rx[r->first_p], s_rx[r->first_p + 1U], s_rx[r->first_p + 2U],
                     ex104_expect(r->first_p), ex104_expect(r->first_p + 1U), ex104_expect(r->first_p + 2U));
    if (!r->ok_c) {
        static char got[2U * CA_SPDIFRX_CS_BYTES + 1U], want[2U * CA_SPDIFRX_CS_BYTES + 1U];
        for (u32 i = 0U; i < CA_SPDIFRX_CS_BYTES; i++) {
            ca_snprintf(&got[2U * i], 3U, "%02X", (u32)s_cs_rx[i]);
            ca_snprintf(&want[2U * i], 3U, "%02X", (u32)s_cs[i]);
        }
        CLI_PRINTF_N("SPDIF   cs got  %s", got);
        CLI_PRINTF_N("SPDIF   cs want %s", want);
    }
}

static void ex104_test(void)
{
    ex104_run_t r = { 0 };
    const bool ok = ex104_run(&r);
    if (r.fs == 0U || r.clk == 0U) { CLI_PUT_N("SPDIF not available with this configuration"); return; }
    ex104_print(&r, ok);
    if (r.skip) { CLI_PUT_N("SPDIF TEST SKIP"); return; }
    if (!ok && r.meas == 0U)
        CLI_PUT_N("SPDIF   no stream synchronized (jumper? try 'wire')");
    CLI_PUT_N(ok ? "SPDIF TEST PASS" : "SPDIF TEST FAIL");
}

static void ex104_all(void)
{
    static const u32 rates[] = { 32000U, 44100U, 48000U, 96000U };
    const u32 keep = s_fs_req;
    u32 runs = 0U, good = 0U;

    for (u32 f = 0U; f < 4U; f++) {
        s_fs_req = rates[f];
        ex104_run_t r = { 0 };
        const bool ok = ex104_run(&r);
        ex104_print(&r, ok);
        if (r.skip) continue;
        runs++;
        if (ok) good++;
    }
    s_fs_req = keep;
    CLI_PRINTF_N("SPDIF ALL %s  (%u/%u)", (good == runs && runs != 0U) ? "PASS" : "FAIL", good, runs);
}

/* DIGITAL continuity of the jumper, as ex103: the SAI is stopped, the pins
 * become plain GPIO, and the board reboots */
static void ex104_wire(void)
{
    ca_sai_stop();
    ca_spdifrx_stop();
    ca_gpio_init_port(CA_GPIO_PORT_E);
    ca_gpio_init_port(CA_GPIO_PORT_D);
    ca_gpio_configure(CA_GPIO_PORT_E, CA_GPIO_PIN_6, CA_GPIO_MODE_OUTPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_configure(CA_GPIO_PORT_D, CA_GPIO_PIN_7, CA_GPIO_MODE_INPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

    ca_gpio_write(CA_GPIO_PORT_E, CA_GPIO_PIN_6, 0);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 lo = ca_gpio_read(CA_GPIO_PORT_D, CA_GPIO_PIN_7);
    ca_gpio_write(CA_GPIO_PORT_E, CA_GPIO_PIN_6, 1);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 hi = ca_gpio_read(CA_GPIO_PORT_D, CA_GPIO_PIN_7);
    const bool ok = (lo == 0U && hi == 1U);

    CLI_PUT_N("  " WIRE_TXT);
    CLI_PRINTF_N("  drive0->read%u  drive1->read%u  %s", lo, hi,
                 ok ? "OK (jumper present)" : "FAIL (jumper missing or wrong)");
    /* The line the bench reads, identical in every example with `wire` */
    CLI_PRINTF_N("  WIRE RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
    CLI_PUT_N("  rebooting in 2.5 s to give the pins back to the SAI...");
    ca_os_sleep(2500);
    ca_os_reset();
}

#if ENABLE_CLI
static void ex104_usage(void)
{
    CLI_PUT_N("spdif     : test [fs] | all | info");
    CLI_PUT_N("wire      : SD_A -> SPDIFRX_IN jumper continuity as plain GPIO, then reboot");
}

static bool ex104_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { ex104_usage(); return false; }
    if (ca_strncmp(p, "wire", 4) == 0) { ex104_wire(); return true; }
    if (ca_strncmp(p, "spdif", 5) != 0) return false;
    p = ca_skip_sp(p + 5);

    if (ca_strncmp(p, "all", 3) == 0) { ex104_all(); return true; }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("SPDIF fs %u -> SAI %u Hz, SPDIFRX_CLK %u Hz, measured %u Hz, errors %02X",
                     s_fs_req, ca_sai_fs(), s_clk, ca_spdifrx_fs(), ca_spdifrx_errors());
        CLI_PUT_N("SPDIF " WIRE_TXT);
        return true;
    }
    if (ca_strncmp(p, "test", 4) == 0) {
        p = ca_skip_sp(p + 4);
        if (*p >= '0' && *p <= '9') s_fs_req = (u32)ca_strtol(p, 10);
        ex104_test();
        return true;
    }
    ex104_usage();
    return true;
}
static ca_cli_node_t s_cli = { ex104_cli, NULL };
#endif

PRIVATE_STRING(why, "ex104: this part has no SPDIFRX (F446, F767 and H753 have it)");

void ca_os_configure(void)
{
    s_clk = ca_spdifrx_init();
    if (s_clk == 0U) {
        /* The line the bench matches to know this example cannot run on
           this part. The reason follows it. */
        CA_EXAMPLE_UNSUPPORTED();
        PUT_N(&priv_why);
        return;
    }
    ca_dma_init();

    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_cli);
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#else  /* EN_SPDIFRX && EN_SAI && EN_DMA */

PRIVATE_STRING(why, "ex104: built without EN_SPDIFRX/EN_SAI/EN_DMA");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_SPDIFRX && EN_SAI && EN_DMA */
