// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex101.c
 * @brief          : Example 101 — backup SRAM survives a reset (bkp.h)
 ******************************************************************************
 * @details
 * The backup SRAM (4 KB on F4/F7/H7/H563, 2 KB on U575) is in the backup
 * domain: a system reset, a watchdog reset and Standby (with the backup
 * regulator on) leave it alone. On the Nucleo VBAT is tied to VDD, so a power
 * cycle clears it.
 *
 *   bks info          size, backup regulator (retention in Standby/VBAT)
 *   bks fill <seed>   whole SRAM <- pseudo-random words from seed
 *   bks check <seed>  verify them: "BKS CHECK PASS" or the first mismatch
 *   bks bounds        out-of-range accesses refused: "BKS BOUNDS PASS"
 *
 * The bench runs fill, then 'reset' (and 'reboot', an IWDG reset), then check.
 ******************************************************************************
 */

#include "caos.h"

#define BKS_CHUNK  64U

static u32 bks_next(u32 *x)                 /* xorshift32 */
{
    *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5;
    return *x;
}

static void bks_fill(u32 seed)
{
    const u32 size = ca_bkp_sram_size();
    u32 x = seed ^ 0xCA05CA05U, buf[BKS_CHUNK / 4U];
    for (u32 off = 0U; off < size; off += BKS_CHUNK) {
        for (u32 i = 0U; i < BKS_CHUNK / 4U; i++) buf[i] = bks_next(&x);
        (void)ca_bkp_sram_write(off, buf, BKS_CHUNK);
    }
    /* Unaligned edges, written last: they go through the driver's word
     * read-modify-write, and the tail is exactly what H753 lost to a reset
     * when the driver still stored bytes. */
    const u8 head[2] = { (u8)seed, (u8)(seed >> 8) };
    const u8 tail[3] = { (u8)~seed, (u8)(~seed >> 8), (u8)(~seed >> 16) };
    (void)ca_bkp_sram_write(1U, head, 2U);
    (void)ca_bkp_sram_write(size - 3U, tail, 3U);
    CLI_PRINTF_N("BKS FILL %u bytes, seed %u", size, seed);
}

/* Byte that bks_fill left at off, edges included */
static u8 bks_expect(u32 seed, u32 size, u32 off, u32 word)
{
    if (off == 1U || off == 2U)  return (u8)(seed >> ((off - 1U) * 8U));
    if (off >= size - 3U)        return (u8)(~seed >> ((off - (size - 3U)) * 8U));
    return (u8)(word >> ((off % 4U) * 8U));
}

static void bks_check(u32 seed)
{
    const u32 size = ca_bkp_sram_size();
    u32 x = seed ^ 0xCA05CA05U, buf[BKS_CHUNK / 4U];
    for (u32 off = 0U; off < size; off += BKS_CHUNK) {
        if (!ca_bkp_sram_read(off, buf, BKS_CHUNK)) {
            CLI_PRINTF_N("BKS CHECK FAIL read refused at %u", off);
            return;
        }
        for (u32 i = 0U; i < BKS_CHUNK / 4U; i++) {
            const u32 gen = bks_next(&x);
            const u32 at  = off + i * 4U;
            u32 want = 0U;
            for (u32 b = 0U; b < 4U; b++)
                want |= (u32)bks_expect(seed, size, at + b, gen) << (b * 8U);
            if (buf[i] != want) {
                CLI_PRINTF_N("BKS CHECK FAIL at %u: %08X want %08X", at, buf[i], want);
                return;
            }
        }
    }
    CLI_PRINTF_N("BKS CHECK PASS %u bytes, seed %u", size, seed);
}

static void bks_bounds(void)
{
    const u32 size = ca_bkp_sram_size();
    u8 b[4] = { 0 };
    const bool ok = ca_bkp_sram_read(size - 4U, b, 4U)       /* last word: allowed */
                 && !ca_bkp_sram_read(size - 3U, b, 4U)      /* crosses the end */
                 && !ca_bkp_sram_write(size, b, 1U)          /* past the end */
                 && !ca_bkp_sram_read(0xFFFFFFF0U, b, 32U)   /* off + len wraps */
                 && !ca_bkp_sram_read(0U, NULL, 4U);
    CLI_PUT_N(ok ? "BKS BOUNDS PASS" : "BKS BOUNDS FAIL");
}

#if ENABLE_CLI
static bool ex101_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("bks: info | fill <seed> | check <seed> | bounds"); return false; }
    if (ca_strncmp(p, "bks", 3) != 0) return false;
    p = ca_skip_sp(p + 3);
    if (ca_bkp_sram_size() == 0U) { CLI_PUT_N("BKS: no backup SRAM on this part"); return true; }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("BKS size %u bytes, retention in Standby/VBAT: %s", ca_bkp_sram_size(),
                     ca_bkp_sram_retain() ? "on" : "off (regulator not available)");
    } else if (ca_strncmp(p, "fill", 4) == 0) {
        bks_fill((u32)ca_strtol(ca_skip_sp(p + 4), 10));
    } else if (ca_strncmp(p, "check", 5) == 0) {
        bks_check((u32)ca_strtol(ca_skip_sp(p + 5), 10));
    } else if (ca_strncmp(p, "bounds", 6) == 0) {
        bks_bounds();
    } else {
        CLI_PUT_N("bks: info | fill <seed> | check <seed> | bounds");
    }
    return true;
}
static ca_cli_node_t s_cli = { ex101_cli, NULL };
#endif

PRIVATE_STRING(why, "ex101: no backup SRAM on this part");

void ca_os_configure(void)
{
    if (ca_bkp_sram_size() == 0U) {
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
