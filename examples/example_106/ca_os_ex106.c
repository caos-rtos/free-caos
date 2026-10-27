// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex106.c
 * @brief          : Example 106 — JPEG codec, held to what the PC gets (F767,
 *                   H753)
 ******************************************************************************
 * @details
 * JPEG is lossy: nothing to compare bit for bit. The board is held to the PC
 * instead. jpeg_check.py --gen encoded the example's synthetic image with
 * Pillow (libjpeg) in six configurations, decoded it back and wrote the
 * files and the PSNR it measured into jpeg_ref.h. The quantization tables
 * are the same Annex K ones on both sides, scaled the same way, so the board
 * must land within 2 dB of the PC.
 *
 *   jpeg test roundtrip   the board encodes and decodes each configuration:
 *                         header read back = configuration, PSNR vs the PC's
 *   jpeg test foreign     the board decodes the six Pillow files
 *   jpeg test determ      the same image encoded twice: same bytes
 *   jpeg test errors      truncated and corrupt streams end on time with an
 *                         error, and the next decode works
 *   jpeg all              the four: "JPEG ALL PASS (4/4)"
 *   jpeg dump             the board's six JPEG files in hex, for
 *                         jpeg_check.py <capture> to open with Pillow
 *   jpeg bench            cycles of encode, decode and the RGB<->MCU steps
 ******************************************************************************
 */

#include "caos.h"

/* Built only where the codec exists: elsewhere the tables and buffers would
 * not even fit (F446), and the unsupported branch says why */
#if EN_JPEG && defined(JPEG)

#include "jpeg_ref.h"

#define IMG_W       64U
#define IMG_H       48U
#define MARGIN_CDB  200U            /* 2 dB, as jpeg_check.py MARGIN_DB */
#define TMO_MS      200U

#define IMG_BYTES   (IMG_W * IMG_H * 3U)
#define MCU_BYTES   (IMG_W * IMG_H * 3U)    /* 4:4:4 is the largest */
#define OUT_BYTES   16384U

static u8 s_img[IMG_BYTES];         /* the source */
static u8 s_back[IMG_BYTES];        /* decoded and converted back */
static u8 s_mcu[MCU_BYTES];
static u8 s_mcu2[MCU_BYTES];
static u8 s_jpg[OUT_BYTES];
static u8 s_jpg2[OUT_BYTES];

static const char *const s_css_name[] = { "gray", "444", "422", "420" };

/* The synthetic image: gradients, a blue checkerboard of 8 x 8 squares and
 * hashed noise, integers only - jpeg_check.py pixel() draws the same */
static void ex106_image(u16 w, u16 h, ca_jpeg_css_t css)
{
    for (u32 y = 0U; y < h; y++)
        for (u32 x = 0U; x < w; x++) {
            u32 hs = (x * 73856093U) ^ (y * 19349663U);
            hs *= 2654435761U;
            const i32 noise = (i32)((hs >> 28) & 15U) - 8;
            i32 r = (i32)(x * 255U / (w - 1U)) + noise;
            i32 g = (i32)(y * 255U / (h - 1U)) - noise;
            const i32 b = (((x / 8U) + (y / 8U)) & 1U) ? 200 : 40;
            r = (r < 0) ? 0 : (r > 255) ? 255 : r;
            g = (g < 0) ? 0 : (g > 255) ? 255 : g;
            if (css == CA_JPEG_GRAY) {
                s_img[y * w + x] = (u8)((r * 3 + g * 6 + b) / 10);
            } else {
                u8 *p = &s_img[3U * (y * w + x)];
                p[0] = (u8)r; p[1] = (u8)g; p[2] = (u8)b;
            }
        }
}

/* 10 log10(x) = log2(x) * 3.0103: log2 from the float's exponent and an
 * atanh series on the mantissa (no libm in caOS) */
static float ex106_log2(float x)
{
    union { float f; u32 i; } u = { x };
    const float e = (float)((i32)((u.i >> 23) & 0xFFU) - 127);
    u.i = (u.i & 0x007FFFFFU) | 0x3F800000U;
    const float t = (u.f - 1.0f) / (u.f + 1.0f), t2 = t * t;
    const float ln = 2.0f * t * (1.0f + t2 * (1.0f / 3.0f + t2 * (1.0f / 5.0f + t2 * (1.0f / 7.0f + t2 / 9.0f))));
    return e + ln * 1.44269504f;
}

/* PSNR of s_back against s_img, dB x 100 */
static u32 ex106_psnr(u32 bytes)
{
    u64 sse = 0U;
    for (u32 i = 0U; i < bytes; i++) {
        const i32 d = (i32)s_img[i] - (i32)s_back[i];
        sse += (u64)(d * d);
    }
    if (sse == 0U) return 9999U;
    const float ratio = (65025.0f * (float)bytes) / (float)sse;
    return (u32)(ex106_log2(ratio) * 301.03f + 0.5f);
}

static u32 ex106_bytes(const ex106_ref_t *r)
{
    return (u32)r->w * r->h * ((r->css == CA_JPEG_GRAY) ? 1U : 3U);
}

static u32 s_shown;

static bool ex106_report(const char *grp, const ex106_ref_t *r, bool ok, u32 psnr, const char *why)
{
    if (!ok || s_shown == 0xFFFFFFFFU)
        CLI_PRINTF_N("JPG   %s %-8s PSNR %u.%02u dB (PC %u.%02u)  %s%s", grp, r->name,
                     psnr / 100U, psnr % 100U, (u32)r->pc_psnr / 100U, (u32)r->pc_psnr % 100U,
                     ok ? "ok" : "BAD ", ok ? "" : why);
    return ok;
}

/* Decode s_jpg[0..len) into s_back and score it against the source */
static bool ex106_decode_score(const char *grp, const ex106_ref_t *r, const u8 *jpg, u32 len)
{
    ca_jpeg_info_t info;
    ex106_image(r->w, r->h, (ca_jpeg_css_t)r->css);
    const bool ok = ca_jpeg_decode(jpg, len, s_mcu2, sizeof(s_mcu2), &info, TMO_MS);
    if (!ok) {
        static char why[48];
        ca_snprintf(why, sizeof(why), "decode failed, errors %02X", ca_jpeg_errors());
        return ex106_report(grp, r, false, 0U, why);
    }
    if (info.width != r->w || info.height != r->h || (u32)info.css != r->css) {
        static char why[64];
        ca_snprintf(why, sizeof(why), "header %ux%u %s, want %ux%u %s", info.width, info.height,
                    s_css_name[info.css], r->w, r->h, s_css_name[r->css]);
        return ex106_report(grp, r, false, 0U, why);
    }
    ca_jpeg_mcu_to_rgb(s_mcu2, r->w, r->h, (ca_jpeg_css_t)r->css, s_back);
    const u32 p = ex106_psnr(ex106_bytes(r));
    return ex106_report(grp, r, p + MARGIN_CDB >= r->pc_psnr, p, "more than 2 dB below the PC");
}

static bool ex106_encode(const ex106_ref_t *r, u8 *out, u32 *len)
{
    const ca_jpeg_cfg_t cfg = { r->w, r->h, (ca_jpeg_css_t)r->css, r->quality };
    ex106_image(r->w, r->h, cfg.css);
    ca_jpeg_rgb_to_mcu(s_img, cfg.width, cfg.height, cfg.css, s_mcu);
    return ca_jpeg_encode(s_mcu, &cfg, out, OUT_BYTES, len, TMO_MS);
}

#define N_REFS (sizeof(s_refs) / sizeof(s_refs[0]))

typedef struct { u32 good, runs; } tally_t;

static tally_t g_roundtrip(void)
{
    tally_t t = { 0 };
    for (u32 i = 0U; i < N_REFS; i++) {
        u32 len = 0U;
        bool ok = ex106_encode(&s_refs[i], s_jpg, &len);
        if (!ok) {
            static char why[48];
            ca_snprintf(why, sizeof(why), "encode failed, errors %02X", ca_jpeg_errors());
            ok = ex106_report("roundtrip", &s_refs[i], false, 0U, why);
        } else {
            ok = ex106_decode_score("roundtrip", &s_refs[i], s_jpg, len);
        }
        t.runs++; if (ok) t.good++;
    }
    return t;
}

static tally_t g_foreign(void)
{
    tally_t t = { 0 };
    for (u32 i = 0U; i < N_REFS; i++) {
        const bool ok = ex106_decode_score("foreign", &s_refs[i], s_refs[i].data, s_refs[i].len);
        t.runs++; if (ok) t.good++;
    }
    return t;
}

static tally_t g_determ(void)
{
    tally_t t = { 0 };
    for (u32 i = 0U; i < N_REFS; i++) {
        u32 a = 0U, b = 0U;
        bool ok = ex106_encode(&s_refs[i], s_jpg, &a) && ex106_encode(&s_refs[i], s_jpg2, &b) && a == b;
        for (u32 k = 0U; ok && k < a; k++) ok = (s_jpg[k] == s_jpg2[k]);
        if (!ok) CLI_PRINTF_N("JPG   determ %-8s %u and %u bytes, not identical", s_refs[i].name, a, b);
        t.runs++; if (ok) t.good++;
    }
    return t;
}

static tally_t g_errors(void)
{
    tally_t t = { 0 };
    const ex106_ref_t *r = &s_refs[3];          /* 420_q90 */
    ca_jpeg_info_t info;

    /* Half the file: the decoder waits for data that never comes */
    u64 t0 = ca_os_tick_64_ms();
    bool ok = !ca_jpeg_decode(r->data, r->len / 2U, s_mcu2, sizeof(s_mcu2), &info, 50U)
           && (ca_jpeg_errors() & CA_JPEG_ERR_TMO) != 0U && ca_os_tick_64_ms() - t0 < 200U;
    if (!ok) CLI_PRINTF_N("JPG   errors truncated: errors %02X", ca_jpeg_errors());
    t.runs++; if (ok) t.good++;

    /* SOF0 turned into SOF2 (progressive, which the codec does not do) */
    for (u32 k = 0U; k < r->len; k++) s_jpg[k] = r->data[k];
    for (u32 k = 0U; k + 1U < r->len; k++)
        if (s_jpg[k] == 0xFFU && s_jpg[k + 1U] == 0xC0U) { s_jpg[k + 1U] = 0xC2U; break; }
    ok = !ca_jpeg_decode(s_jpg, r->len, s_mcu2, sizeof(s_mcu2), &info, 50U);
    if (!ok) CLI_PUT_N("JPG   errors SOF2: decoded as if it were baseline");
    t.runs++; if (ok) t.good++;

    /* The codec is not left wedged */
    ok = ex106_decode_score("errors after", r, r->data, r->len);
    t.runs++; if (ok) t.good++;
    return t;
}

typedef struct { const char *name; tally_t (*run)(void); } group_t;

static const group_t s_groups[] = {
    { "roundtrip", g_roundtrip }, { "foreign", g_foreign }, { "determ", g_determ }, { "errors", g_errors },
};
#define N_GROUPS (sizeof(s_groups) / sizeof(s_groups[0]))

static bool run_group(const group_t *g, bool verbose)
{
    s_shown = verbose ? 0xFFFFFFFFU : 0U;
    const tally_t t = g->run();
    const bool ok = t.good == t.runs;
    CLI_PRINTF_N("JPG %-9s %2u/%2u  %s", g->name, t.good, t.runs, ok ? "PASS" : "FAIL");
    return ok;
}

static void ex106_all(void)
{
    u32 good = 0U;
    for (u32 i = 0U; i < N_GROUPS; i++) if (run_group(&s_groups[i], false)) good++;
    CLI_PRINTF_N("JPEG ALL %s  (%u/%u)", (good == N_GROUPS) ? "PASS" : "FAIL", good, (u32)N_GROUPS);
}

/* The board's files in hex, 32 bytes a line, for jpeg_check.py */
static void ex106_dump(void)
{
    for (u32 i = 0U; i < N_REFS; i++) {
        const ex106_ref_t *r = &s_refs[i];
        u32 len = 0U;
        if (!ex106_encode(r, s_jpg, &len)) { CLI_PRINTF_N("JPG dump %s: encode failed", r->name); continue; }
        CLI_PRINTF_N("JPG BEGIN %s %u %u %s %u %u", r->name, r->w, r->h, s_css_name[r->css], r->quality, len);
        static char line[80];
        for (u32 k = 0U; k < len; k += 32U) {
            u32 n = 0U;
            for (u32 j = k; j < len && j < k + 32U; j++)
                n += (u32)ca_snprintf(&line[n], sizeof(line) - n, "%02X", (u32)s_jpg[j]);
            CLI_PRINTF_N("JPG %04X:%s", k, line);
            ca_os_sleep(2U);                    /* let the console drain */
        }
        CLI_PUT_N("JPG END");
    }
    CLI_PUT_N("JPG DUMP END");
}

static void ex106_bench(void)
{
    const ex106_ref_t *r = &s_refs[3];          /* 420_q90, 64 x 48 */
    const ca_jpeg_cfg_t cfg = { r->w, r->h, CA_JPEG_420, r->quality };
    ca_jpeg_info_t info;
    u32 len = 0U;
    ex106_image(cfg.width, cfg.height, cfg.css);
    ca_os_sleep(30U);                           /* the console sent the echo */

    __disable_irq();
    u32 t0 = DWT->CYCCNT;
    ca_jpeg_rgb_to_mcu(s_img, cfg.width, cfg.height, cfg.css, s_mcu);
    const u32 c_in = DWT->CYCCNT - t0;
    t0 = DWT->CYCCNT;
    const bool e = ca_jpeg_encode(s_mcu, &cfg, s_jpg, OUT_BYTES, &len, TMO_MS);
    const u32 c_enc = DWT->CYCCNT - t0;
    t0 = DWT->CYCCNT;
    const bool d = ca_jpeg_decode(s_jpg, len, s_mcu2, sizeof(s_mcu2), &info, TMO_MS);
    const u32 c_dec = DWT->CYCCNT - t0;
    t0 = DWT->CYCCNT;
    ca_jpeg_mcu_to_rgb(s_mcu2, cfg.width, cfg.height, cfg.css, s_back);
    const u32 c_out = DWT->CYCCNT - t0;
    __enable_irq();

    const u32 px = (u32)cfg.width * cfg.height;
    CLI_PRINTF_N("JPG bench %ux%u 4:2:0 q%u, %u MHz core, %u bytes", cfg.width, cfg.height,
                 cfg.quality, SystemCoreClock / 1000000U, len);
    CLI_PRINTF_N("JPG   rgb->mcu %7u cyc  encode %7u cyc%s  decode %7u cyc%s  mcu->rgb %7u cyc",
                 c_in, c_enc, e ? "" : " (failed)", c_dec, d ? "" : " (failed)", c_out);
    CLI_PRINTF_N("JPG   per pixel: rgb->mcu %u  encode %u  decode %u  mcu->rgb %u",
                 c_in / px, c_enc / px, c_dec / px, c_out / px);
}

#if ENABLE_CLI
static void ex106_usage(void)
{
    CLI_PUT_N("jpeg      : test <roundtrip|foreign|determ|errors> | all | dump | bench");
}

static bool ex106_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { ex106_usage(); return false; }
    if (ca_strncmp(p, "jpeg", 4) != 0) return false;
    p = ca_skip_sp(p + 4);

    if (ca_strncmp(p, "all", 3) == 0)   { ex106_all(); return true; }
    if (ca_strncmp(p, "dump", 4) == 0)  { ex106_dump(); return true; }
    if (ca_strncmp(p, "bench", 5) == 0) { ex106_bench(); return true; }
    if (ca_strncmp(p, "test", 4) == 0) {
        p = ca_skip_sp(p + 4);
        for (u32 i = 0U; i < N_GROUPS; i++) {
            const u32 n = ca_strlen(s_groups[i].name);
            if (ca_strncmp(p, s_groups[i].name, n) == 0) { (void)run_group(&s_groups[i], true); return true; }
        }
    }
    ex106_usage();
    return true;
}
static ca_cli_node_t s_cli = { ex106_cli, NULL };
#endif

PRIVATE_STRING(why, "ex106: this part has no JPEG codec (F767 and H753 have it)");

void ca_os_configure(void)
{
    if (!ca_jpeg_init()) {
        /* The line the bench matches to know this example cannot run on
           this part. The reason follows it. */
        CA_EXAMPLE_UNSUPPORTED();
        PUT_N(&priv_why);
        return;
    }

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

#else  /* EN_JPEG && JPEG */

PRIVATE_STRING(why, "ex106: no JPEG codec on this part, or built without EN_JPEG (F767 and H753 have it)");

void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_why);
}

void ca_os_main(void)
{
    ca_os_run(true);
}

#endif /* EN_JPEG && JPEG */
