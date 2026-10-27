// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex103.c
 * @brief          : Example 103 — SAI1 loopback, polled and DMA (F446, F767,
 *                   H753, H563, U575)
 ******************************************************************************
 * @details
 * SAI1 as the driver sets it up for a codec: block A master transmitter,
 * block B synchronous slave receiver. The SAI has no internal data loopback
 * (the two blocks share SCK and FS inside, not the data lines), so one jumper
 * closes the loop, the same on all five Nucleo:
 *
 *     PE6 SD_A  CN9-20  ---->  PE3 SD_B  CN9-22
 *
 *   sai test [fs] [bits] [i2s|msb]   one configuration, three ways:
 *                            polled, one DMA block, circular DMA stream.
 *                            Every received word must equal the one sent
 *                            (after a lag of a few words, found and shown),
 *                            and the sample rate measured with the CPU clock
 *                            must match the one ca_sai_init() declared.
 *   sai all                  5 rates x 16/24/32 bits x I2S/MSB, one verdict:
 *                            "SAI ALL PASS" / "SAI ALL FAIL"
 *   sai info                 the configuration and the error flags
 *   wire                     the jumper as plain GPIO, then reboot
 *
 * The sample rate is approximated with the clocks already running (sai.h):
 * the measured one checks the clock arithmetic, not the audio quality. On
 * H7 the SAI runs on the HSI and the CPU on the HSE: the two differ by the
 * HSI's own error.
 ******************************************************************************
 */

#include "caos.h"

#if EN_SAI && EN_DMA

#if MCU_ID == 103
/* 20 KB of RAM and no SAI: the example stops at boot, the buffers only
 * have to link */
#define EX103_WORDS     256U
#else
#define EX103_WORDS     2048U         /* 1024 stereo frames */
#endif
#define EX103_LAG_MAX   8U            /* words the receiver may trail by */
#define EX103_FS_TOL_PM 20U           /* measured fs within 2% (H7: HSI vs HSE) */

#define WIRE_TXT        "PE6 SD_A CN9-20 ---- PE3 SD_B CN9-22"

/* Static, not on the stack: on H7 the stacks are in DTCM, out of DMA reach */
static i32 s_tx[EX103_WORDS];
static i32 s_rx[EX103_WORDS];

static ca_sai_config_t s_cfg = { 48000U, 24U, CA_SAI_I2S };

static u32 ex103_mask(void)
{
    return (s_cfg.bits == 32U) ? 0xFFFFFFFFU : ((1U << s_cfg.bits) - 1U);
}

/* Pseudo-random words of `bits` bits, sign-extended as the driver returns them */
static void ex103_fill(void)
{
    u32 r = 0x2545F491U;
    const u32 sh = 32U - s_cfg.bits;
    for (u32 i = 0U; i < EX103_WORDS; i++) {
        r = r * 1664525U + 1013904223U;
        s_tx[i] = (i32)r >> sh;
        s_rx[i] = 0;
    }
}

/* Lag of rx behind tx and the number of words that differ after it.
 * `circular`: the stream wraps, so compare modulo the buffer. */
static bool ex103_check(bool circular, u32 *lag, u32 *bad)
{
    const u32 m = ex103_mask();
    *lag = 0xFFFFFFFFU;
    for (u32 l = 0U; l <= EX103_LAG_MAX && *lag == 0xFFFFFFFFU; l++) {
        bool same = true;
        for (u32 k = 0U; k < 32U && same; k++)
            same = (((u32)s_rx[(l + k) % EX103_WORDS] ^ (u32)s_tx[k]) & m) == 0U;
        if (same) *lag = l;
    }
    if (*lag == 0xFFFFFFFFU) { *bad = EX103_WORDS; return false; }

    const u32 n = circular ? EX103_WORDS : EX103_WORDS - *lag;
    *bad = 0U;
    for (u32 k = 0U; k < n; k++)
        if ((((u32)s_rx[(*lag + k) % EX103_WORDS] ^ (u32)s_tx[k]) & m) != 0U) (*bad)++;
    return *bad == 0U;
}

/* Frames per second from a transfer of `words` timed in CPU cycles */
static u32 ex103_fs_meas(u32 words, u32 cycles)
{
    return (cycles == 0U) ? 0U
         : (u32)(((u64)(words / 2U) * SystemCoreClock) / cycles);
}

static bool ex103_fs_ok_pm(u32 a, u32 fs, u32 tol_pm)
{
    const u32 d = (a > fs) ? a - fs : fs - a;
    return (u64)d * 1000U <= (u64)fs * tol_pm;
}

static bool ex103_fs_ok(u32 meas, u32 fs)
{
    return ex103_fs_ok_pm(meas, fs, EX103_FS_TOL_PM);
}

typedef struct {
    u32 fs, meas;
    u32 lag_p, bad_p, lag_d, bad_d, lag_s, bad_s;
    bool ok_p, ok_d, ok_s, ok_f;
    u32 err;
} ex103_run_t;

static bool ex103_run(ex103_run_t *r)
{
    r->fs = ca_sai_init(&s_cfg);
    if (r->fs == 0U) return false;

    /* Polled. The console first drains what was just printed (the command's
     * echo, the previous run's line): its task holding the CPU for two
     * characters, 187 us, overran the 8-word receive FIFO at 48 kHz (83 us)
     * - polled passes at 96 kHz once the console is quiet (H563, 25/10). */
    ex103_fill();
    ca_os_sleep(30U);
    const u32 got = ca_sai_xfer(s_tx, s_rx, EX103_WORDS, 2000U);
    r->ok_p = (got == EX103_WORDS) && ex103_check(false, &r->lag_p, &r->bad_p);

    /* One DMA block */
    ex103_fill();
    r->ok_d = ca_sai_xfer_dma(s_tx, s_rx, EX103_WORDS, 2000U)
           && ex103_check(false, &r->lag_d, &r->bad_d);

    /* Circular stream, and the sample rate measured on it: the DMA write
     * position every 5 ms for 100 ms (less than one buffer between two reads
     * even at 96 kHz), its advance summed across the wraps, over CPU cycles.
     * Timing a single transfer instead counted its start and the
     * sign-extension loop that follows it: 0.4% low at 16/24 bits.
     * The 5 ms are spun on CYCCNT, not slept: on the M7 the counter stops
     * in the idle task's WFI (H753 read 25 500 cycles per 5 ms sleep, a
     * sample rate 92 times too high); on the M33 it keeps counting. */
    ex103_fill();
    r->ok_s = false;
    r->lag_s = r->bad_s = 0U;
    r->meas = 0U;
    if (ca_sai_stream_start(s_tx, s_rx, EX103_WORDS)) {
        ca_os_sleep(5U);
        u32 prev = ca_sai_stream_pos();
        const u32 t1 = DWT->CYCCNT;
        u32 total = 0U;
        const u32 step = SystemCoreClock / 200U;      /* 5 ms */
        for (u32 k = 0U; k < 20U; k++) {
            while ((DWT->CYCCNT - t1) < (k + 1U) * step) { }
            const u32 pos = ca_sai_stream_pos();
            total += (pos + EX103_WORDS - prev) % EX103_WORDS;
            prev = pos;
        }
        const u32 cyc_s = DWT->CYCCNT - t1;
        /* At least two whole buffers before the check: at 8 kHz the 100 ms
         * above are 1600 words, less than one turn of the 2048 */
        ca_os_sleep(2U * (EX103_WORDS / 2U) * 1000U / r->fs + 5U);
        ca_sai_stop();
        r->meas = ex103_fs_meas(total, cyc_s);
        r->ok_s = (total != 0U) && ex103_check(true, &r->lag_s, &r->bad_s);
    }
    /* The rate measured is the one declared, and the one declared is within
     * 1% of the one asked for (U5 gave 9920 Hz for 8000 before NODIV = 0) */
    r->ok_f = ex103_fs_ok(r->meas, r->fs) && ex103_fs_ok_pm(r->fs, s_cfg.fs_hz, 10U);
    r->err = ca_sai_errors() & ~CA_SAI_ERR_UDR;  /* underrun: the stop tail */
    return r->ok_p && r->ok_d && r->ok_s && r->ok_f && r->err == 0U;
}

static void ex103_print(const ex103_run_t *r, bool ok)
{
    CLI_PRINTF_N("SAI %6u -> %6u Hz (meas %6u) %2u bit %s  poll %s/%u  dma %s/%u  stream %s/%u  err %02X  %s",
                 s_cfg.fs_hz, r->fs, r->meas, (u32)s_cfg.bits,
                 (s_cfg.proto == CA_SAI_MSB) ? "msb" : "i2s",
                 r->ok_p ? "ok" : "BAD", r->lag_p, r->ok_d ? "ok" : "BAD", r->lag_d,
                 r->ok_s ? "ok" : "BAD", r->lag_s, r->err, ok ? "PASS" : "FAIL");
}

static void ex103_test(void)
{
    ex103_run_t r = { 0 };
    const bool ok = ex103_run(&r);
    if (r.fs == 0U) { CLI_PUT_N("SAI not available with this configuration"); return; }
    ex103_print(&r, ok);
    if (!r.ok_p || !r.ok_d || !r.ok_s)
        CLI_PRINTF_N("SAI   words differing: poll %u  dma %u  stream %u (jumper? try 'wire')",
                     r.bad_p, r.bad_d, r.bad_s);
    CLI_PUT_N(ok ? "SAI TEST PASS" : "SAI TEST FAIL");
}

static void ex103_all(void)
{
    static const u32 rates[] = { 8000U, 16000U, 44100U, 48000U, 96000U };
    static const u8  bits[]  = { 16U, 24U, 32U };
    const ca_sai_config_t keep = s_cfg;
    u32 runs = 0U, good = 0U;

    for (u32 p = 0U; p < 2U; p++) {
        for (u32 b = 0U; b < 3U; b++) {
            for (u32 f = 0U; f < 5U; f++) {
                s_cfg.fs_hz = rates[f];
                s_cfg.bits  = bits[b];
                s_cfg.proto = (p == 0U) ? CA_SAI_I2S : CA_SAI_MSB;
                ex103_run_t r = { 0 };
                const bool ok = ex103_run(&r);
                ex103_print(&r, ok);
                runs++;
                if (ok) good++;
            }
        }
    }
    s_cfg = keep;
    CLI_PRINTF_N("SAI ALL %s  (%u/%u)", (good == runs) ? "PASS" : "FAIL", good, runs);
}

/* DIGITAL continuity of the jumper, as ex27: a missing wire and a broken
 * SAI give the same "words differ", this tells them apart. The SAI is
 * stopped, the pins become plain GPIO, and the board reboots. */
static void ex103_wire(void)
{
    ca_sai_stop();
    ca_gpio_init_port(CA_GPIO_PORT_E);
    ca_gpio_configure(CA_GPIO_PORT_E, CA_GPIO_PIN_6, CA_GPIO_MODE_OUTPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_configure(CA_GPIO_PORT_E, CA_GPIO_PIN_3, CA_GPIO_MODE_INPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

    ca_gpio_write(CA_GPIO_PORT_E, CA_GPIO_PIN_6, 0);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 lo = ca_gpio_read(CA_GPIO_PORT_E, CA_GPIO_PIN_3);
    ca_gpio_write(CA_GPIO_PORT_E, CA_GPIO_PIN_6, 1);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    const u8 hi = ca_gpio_read(CA_GPIO_PORT_E, CA_GPIO_PIN_3);
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
static const char *ex103_num(const char *p, u32 *v)
{
    p = ca_skip_sp(p);
    if (*p < '0' || *p > '9') return NULL;
    *v = (u32)ca_strtol(p, 10);
    while (*p >= '0' && *p <= '9') p++;
    return p;
}

static void ex103_usage(void)
{
    CLI_PUT_N("sai       : test [fs] [16|24|32] [i2s|msb] | all | info");
    CLI_PUT_N("wire      : SD_A -> SD_B jumper continuity as plain GPIO, then reboot");
}

static bool ex103_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { ex103_usage(); return false; }
    if (ca_strncmp(p, "wire", 4) == 0) { ex103_wire(); return true; }
    if (ca_strncmp(p, "sai", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "all", 3) == 0) { ex103_all(); return true; }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("SAI fs %u -> %u Hz, %u bit, %s, errors %02X", s_cfg.fs_hz, ca_sai_fs(),
                     (u32)s_cfg.bits, (s_cfg.proto == CA_SAI_MSB) ? "msb" : "i2s", ca_sai_errors());
        CLI_PUT_N("SAI " WIRE_TXT);
        return true;
    }
    if (ca_strncmp(p, "test", 4) == 0) {
        u32 v = 0U;
        const char *q = ex103_num(p + 4, &v);
        if (q != NULL) {
            s_cfg.fs_hz = v;
            const char *q2 = ex103_num(q, &v);
            if (q2 != NULL) { s_cfg.bits = (u8)v; q = q2; }
            q = ca_skip_sp(q);
            if (ca_strncmp(q, "msb", 3) == 0) s_cfg.proto = CA_SAI_MSB;
            if (ca_strncmp(q, "i2s", 3) == 0) s_cfg.proto = CA_SAI_I2S;
        }
        ex103_test();
        return true;
    }
    ex103_usage();
    return true;
}
static ca_cli_node_t s_cli = { ex103_cli, NULL };
#endif

PRIVATE_STRING(why, "ex103: this part has no SAI1 (F446, F767, H753, H563 and U575 have it)");

void ca_os_configure(void)
{
    if (ca_sai_init(&s_cfg) == 0U) {
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

#else  /* EN_SAI && EN_DMA */

PRIVATE_STRING(why, "ex103: built without EN_SAI/EN_DMA");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_SAI && EN_DMA */
