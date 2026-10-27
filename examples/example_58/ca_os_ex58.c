// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 58 — FatFS on SD card via SDIO — Interactive CLI
 *
 * Hardware: STM32F4VE black board (F407)
 *   microSD in onboard connector (SDIO 4-bit)
 *   Heartbeat LED: PA6
 *   Console: USB CDC (STDIN_ON_USB_CDC=1)
 *
 * The volume commands come from the FAT service (fat_service.c, EN_FAT_SERVICE
 * + EN_CLI_FAT):
 *   fat fdisk [<MB>]   — partition MBR only (default 64MB)
 *   fat mkfs           — format FAT32 the partition created by fdisk
 *   fat mount          — mount volume, print fs_type/size/free
 *   fat umount         — unmount
 *   fat free           — total/free/used space
 *   fat pwd | cd <dir> | ls [dir] | mkdir <dir> | rmdir <dir>
 *   fat touch <file> | rm <file> | cat <file>
 *
 * This example adds the test tools, which are not a service:
 *   fat blob <file> <n> <B|K|M>  — write n bytes/KB/MB (cyclic 0x00..0xFF)
 *   fat sdtest <lba> [<n>]       — raw driver write→read→verify test (DESTRUCTIVE)
 *   fat test_all                 — full unattended sequence (DESTRUCTIVE)
 *   fat log                      — logger counters
 *
 * Logging: on every mount the example mirrors stdout lines to
 * <uptime>_cmd_log.log on the card, via ca_stdout_set_repeater(); it closes
 * the file on umount and before the destructive commands. It hooks the service
 * events (ca_fat_service_set_event_cb) and shares its lock - see the LOGGER
 * section for why both guards are needed (FatFS is FF_FS_REENTRANT=0).
 */

#include "caos.h"

#if EN_FAT && EN_FAT_SERVICE && EN_SDIO && EN_DMA && EN_RTC

#include "ff.h"

/* ============================================================================
 * Example context — STATIC, in .bss
 * --------------------------------------------------------------------------
 * Task stacks cannot carry FatFS objects (FIL/FILINFO ~1 KB with LFN), so
 * they live here. The 4 KB write buffer is NOT here: blob and sdtest borrow
 * the service's aligned work buffer under ca_fat_lock().
 *
 * work_rx is the sdtest read-back, 32-byte aligned for the SDMMC IDMA and
 * SEPARATE from the write buffer: a read DMA not fully torn down could still
 * point at the buffer a write DMA is being armed on.
 * ========================================================================== */
typedef struct {
    u8      work_rx[512]  __attribute__((aligned(32)));  /* sdtest read-back */
    FIL     fil;                     /* blob */
    FILINFO fno;                     /* blob existence check */
    /* CLI parser tokens */
    char    sub[16];
    char    a1[FF_LFN_BUF + 2];
    char    a2[16];
    char    a3[4];

    /* --- stdout -> file logger (see the LOGGER section below) --- */
    FIL     logf;
    bool    logging;                 /* repeater installed and logf open */
    char    logname[32];             /* <uptime>_cmd_log.log */
    u32     log_lines;               /* written, for the sync policy */
    u32     log_lost;                /* dropped: FatFS busy or recursion */
} ex58_ctx_t;

static ex58_ctx_t  s_ctx __attribute__((aligned(32)));
static ex58_ctx_t *g_ctx = &s_ctx;

/* ============================================================================
 * LOGGER — stdout mirrored to a file on the SD card
 * --------------------------------------------------------------------------
 * On CA_FAT_EV_MOUNTED a repeater is installed on stdout
 * (ca_stdout_set_repeater): from then on every line printed by anyone is also
 * appended to <uptime>_cmd_log.log. CA_FAT_EV_UNMOUNTING removes it and closes
 * the file.
 *
 * THE PROBLEM THIS CODE EXISTS TO SOLVE. The repeater runs in the STDOUT
 * DRAIN THREAD, while the fat commands run in the CLI THREAD - two different
 * tasks - and caOS builds FatFS with FF_FS_REENTRANT=0 (caos_ffconf.h): no
 * internal locking whatsoever. Two threads inside FatFS at the same time
 * corrupts the volume, and it would happen on the very first "fat blob"
 * (44 s inside f_write) if anything printed meanwhile.
 *
 * Hence two guards, and they cover different things:
 *
 *  1. ca_fat_lock - the service lock, a recursive mutex that every ca_fat_*
 *     operation takes. The repeater takes it with TRYLOCK, never lock:
 *     blocking here would stall the console for as long as a blob takes, and
 *     stdout would then drop lines anyway - with the queue full and the
 *     system frozen instead of just this one line missing. A refused lock
 *     means "a fat command is running": the line is counted in log_lost and
 *     dropped, which is the honest trade.
 *
 *  2. s_in_repeater (flag) - stops RECURSION, which the mutex cannot: it is
 *     recursive per task, so the drain thread would happily re-enter itself.
 *     Anything printed while we are inside f_write - a FatFS error, a line
 *     from the SDIO driver - comes straight back into the repeater and would
 *     call f_write again, forever. Those lines are lost by construction (they
 *     are the log of the logger failing); losing them is the price of not
 *     deadlocking the console.
 *
 * The destructive commands tear the filesystem out from under an open file,
 * so they stop the logger first: the service raises CA_FAT_EV_DESTRUCTIVE for
 * fdisk/mkfs, and sdtest/test_all call log_stop_for() here.
 * ========================================================================== */

/* Sync every N lines: f_sync rewrites the FAT and the directory entry, so
 * doing it per line makes each print a multi-block write. A reset loses at
 * most the last few lines, which for a console log is an acceptable trade. */
#define LOG_SYNC_EVERY  16

static volatile bool s_in_repeater = false;    /* recursion guard */

/* The repeater itself: called by the stdout drain thread for every line. */
static u16 log_repeater(p_str line)
{
    if (!g_ctx->logging || line == NULL || line->size == 0U) return 0;

    /* Recursion: we are already inside f_write and something printed. */
    if (s_in_repeater) { g_ctx->log_lost++; return 0; }

    /* A fat command holds FatFS: drop the line rather than block the console. */
    if (!ca_fat_trylock()) { g_ctx->log_lost++; return 0; }

    s_in_repeater = true;
    UINT bw = 0;
    /* No newline added here: the _n printers already put \r\n inside
     * str->size before queueing (stdout.c), so appending one wrote every
     * line twice-terminated and doubled the file. */
    FRESULT fr = f_write(&g_ctx->logf, line->data, (UINT)line->size, &bw);
    if (fr == FR_OK) {
        if ((++g_ctx->log_lines % LOG_SYNC_EVERY) == 0U) f_sync(&g_ctx->logf);
    } else {
        g_ctx->log_lost++;
    }
    s_in_repeater = false;

    ca_fat_unlock();
    return line->size;
}

/* Open the log file and install the repeater. The volume is already mounted. */
static void log_start(void)
{
    if (g_ctx->logging) return;

    /* Name from uptime: unique per boot without needing an RTC, and it sorts
     * chronologically. FF_USE_LFN=3 makes the length a non-issue. */
    u32 up = (u32)ca_os_tick_64_ms();
    ca_snprintf(g_ctx->logname, sizeof(g_ctx->logname), "%u_cmd_log.log",
                (unsigned)up);

    ca_fat_lock();
    FRESULT fr = f_open(&g_ctx->logf, g_ctx->logname,
                        FA_WRITE | FA_CREATE_ALWAYS);
    ca_fat_unlock();
    if (fr != FR_OK) {
        ca_stdout_printf_n("[log] cannot open '%s': %s (err %d)",
                           g_ctx->logname, ca_fat_strerror((i32)fr), (int)fr);
        return;
    }
    g_ctx->log_lines = 0;
    g_ctx->log_lost  = 0;
    g_ctx->logging   = true;
    ca_stdout_set_repeater(log_repeater);
    /* Printed AFTER installing: it is the first line of the file too. */
    ca_stdout_printf_n("[log] stdout -> '%s'", g_ctx->logname);
}

/* Remove the repeater and close the file. Safe to call when not logging. */
static void log_stop(void)
{
    if (!g_ctx->logging) return;

    /* Uninstall FIRST: from here on no line can reach f_write, so the close
     * below cannot race with the drain thread. */
    ca_stdout_set_repeater(NULL);
    g_ctx->logging = false;

    ca_fat_lock();
    f_close(&g_ctx->logf);
    ca_fat_unlock();

    ca_stdout_printf_n("[log] closed '%s' - %u lines, %u lost",
                       g_ctx->logname, (unsigned)g_ctx->log_lines,
                       (unsigned)g_ctx->log_lost);
}

/* Destructive commands rewrite the very sectors the open log file lives in.
 * Stop the logger before they run, and say so: silently losing the log would
 * be worse than the one line of noise. */
static void log_stop_for(const char *what)
{
    if (!g_ctx->logging) return;
    ca_stdout_printf_n("[log] '%s' is destructive: logging stopped", what);
    log_stop();
}

/* The service tells us when the volume comes and goes. */
static void fat_event(ca_fat_event_t ev, const char *what)
{
    switch (ev) {
    case CA_FAT_EV_MOUNTED:     log_start();        break;
    case CA_FAT_EV_UNMOUNTING:  log_stop();         break;
    case CA_FAT_EV_DESTRUCTIVE: log_stop_for(what); break;
    default:                                        break;
    }
}

/* ============================================================================
 * Test tools
 * ========================================================================== */

/* fat sdtest <lba> [<n>] — raw SDIO driver test, bypasses FatFS.
 * Phase 1: writes n blocks with deterministic pattern (word = f(lba,idx)).
 * Phase 2: reads back all and verifies (catches read-after-write corruptions).
 * DESTRUCTIVE on sectors [lba, lba+n): use LBA outside the FAT volume. */
static bool cmd_sdtest(const char *lba_s, const char *n_s)
{
    log_stop_for("sdtest");

    u16 idx = 0;
    u32 lba = (u32)ca_strtol_index(lba_s, 10, &idx);
    idx = 0;
    u32 n = (n_s && *n_s) ? (u32)ca_strtol_index(n_s, 10, &idx) : 8U;
    if (n == 0U) n = 1U;

    if (!ca_sdio_init()) { ca_stdout_put_n("[sdtest] init FAIL"); return false; }
    u32 blocks = ca_sdio_get_block_count();
    if (lba + n > blocks) {
        ca_stdout_printf_n("[sdtest] out of range (card=%u blocks)", (unsigned)blocks);
        return false;
    }

    /* The service work buffer is shared: hold its lock while using it. */
    ca_fat_lock();
    u32 *wr = (u32 *)(void *)ca_fat_work_buf(NULL);   /* 512 B, TX */
    u32 *rd = (u32 *)(void *)g_ctx->work_rx;          /* 512 B, RX - distinct */
    bool ok = true;

    /* phase 1: write burst single-block (like FatFS during blob), timed */
    u32 tw = ca_os_tick_ms();
    for (u32 i = 0U; i < n && ok; i++) {
        for (u32 j = 0U; j < 128U; j++)
            wr[j] = ((lba + i) << 16) ^ (j * 0x9E3779B9U);
        if (!ca_sdio_write_block(lba + i, (const u8 *)wr)) {
            ca_stdout_printf_n("[sdtest] WRITE FAIL lba=%u", (unsigned)(lba + i));
            ok = false;
        }
    }
    tw = ca_os_tick_ms() - tw;

    /* phase 2: read back + verify, timed */
    u32 tr = ca_os_tick_ms();
    u32 bad = 0U;
    for (u32 i = 0U; i < n && ok; i++) {
        if (!ca_sdio_read_block(lba + i, (u8 *)rd)) {
            ca_stdout_printf_n("[sdtest] READ FAIL lba=%u", (unsigned)(lba + i));
            ok = false;
            break;
        }
        for (u32 j = 0U; j < 128U; j++) {
            u32 exp = ((lba + i) << 16) ^ (j * 0x9E3779B9U);
            if (rd[j] != exp) {
                if (bad < 4U)
                    ca_stdout_printf_n("[sdtest] MISMATCH lba=%u word=%u exp=%08X got=%08X",
                                       (unsigned)(lba + i), (unsigned)j,
                                       (unsigned)exp, (unsigned)rd[j]);
                bad++;
            }
        }
    }
    tr = ca_os_tick_ms() - tr;
    ca_fat_unlock();
    if (!ok) return false;

    u32 kb = (n * 512U) / 1024U;
    ca_stdout_printf_n("[sdtest] %u blocks (%u KB), %u bad words %s",
                       (unsigned)n, (unsigned)kb, (unsigned)bad, bad ? "FAIL" : "- OK");
    ca_stdout_printf_n("[sdtest] WR single-block: %u ms (%u KB/s) | RD: %u ms (%u KB/s)",
                       (unsigned)tw, (unsigned)(tw ? kb * 1000U / tw : 0),
                       (unsigned)tr, (unsigned)(tr ? kb * 1000U / tr : 0));
    return bad == 0U;
}

/* fat blob <file> <n> <B|K|M> */
static bool cmd_blob(const char *name, const char *size_s, const char *unit_s)
{
    if (!ca_fat_is_mounted()) { ca_stdout_put_n("[fat] not mounted"); return false; }

    /* Calculate size */
    u16 idx = 0;
    u32 n = (u32)ca_strtol_index(size_s, 10, &idx);

    /* The unit may be a separate token ("10 M") or stuck to the number
     * ("10M"). ca_strtol_index stops at the letter and leaves idx on it, so
     * take it from there first: with "10M" the third token is empty and the
     * default 'B' silently wrote 10 bytes instead of 10 MB. */
    char unit = size_s[idx];
    if (unit == '\0') unit = (unit_s && unit_s[0]) ? unit_s[0] : 'B';

    if      (unit == 'K' || unit == 'k') n *= 1024U;
    else if (unit == 'M' || unit == 'm') n *= 1024U * 1024U;
    else if (unit != 'B' && unit != 'b') {
        ca_stdout_printf_n("[fat] blob: unknown unit '%c' (B|K|M)", unit);
        return false;
    }
    if (n == 0) { ca_stdout_put_n("[fat] blob: size 0"); return false; }

    ca_fat_lock();
    if (f_stat(name, &g_ctx->fno) == FR_OK) {
        ca_fat_unlock();
        ca_stdout_printf_n("[fat] %s: already exists", name);
        return false;
    }

    FRESULT fr = f_open(&g_ctx->fil, name, FA_CREATE_NEW | FA_WRITE);
    if (fr != FR_OK) {
        ca_fat_unlock();
        ca_stdout_printf_n("[fat] blob open: %s (err %d)", ca_fat_strerror((i32)fr), (int)fr);
        return false;
    }

    /* Fill the 4 KB work buffer with the cyclic 0x00..0xFF pattern and write
     * in 4 KB chunks (8 sectors) so FatFS passes full multi-block writes to
     * the driver (CMD25 IDMA) instead of sub-sector read-modify-write. */
    u32 wsz = 0;
    u8 *work = ca_fat_work_buf(&wsz);
    for (u32 i = 0; i < wsz; i++) work[i] = (u8)i;

    u32 t0 = ca_os_tick_ms();
    u32 written = 0;
    while (written < n && fr == FR_OK) {
        u32 chunk = n - written;
        if (chunk > wsz) chunk = wsz;
        UINT bw;
        fr = f_write(&g_ctx->fil, work, chunk, &bw);
        written += bw;
    }
    fr = (fr == FR_OK) ? f_close(&g_ctx->fil) : (f_close(&g_ctx->fil), fr);
    ca_fat_unlock();
    u32 ms = ca_os_tick_ms() - t0;

    if (fr != FR_OK) {
        ca_stdout_printf_n("[fat] blob write: %s (err %d)", ca_fat_strerror((i32)fr), (int)fr);
        return false;
    }
    u32 kb = written / 1024U;
    ca_stdout_printf_n("[fat] blob %s  %u bytes  %u ms  (%u KB/s)",
                       name, (unsigned)written, (unsigned)ms,
                       (unsigned)(ms ? kb * 1000U / ms : 0));
    return true;
}

/* ============================================================================
 * fat test_all — full unattended sequence
 * --------------------------------------------------------------------------
 * Runs the raw driver test and then every service operation in an order where
 * each one has something to work on, checking each step instead of just
 * printing it: every step returns true on success.
 *
 * DESTRUCTIVE: fdisk and mkfs wipe the card, and sdtest overwrites raw
 * sectors. It ends with umount, so the card is left clean and the log file
 * closed. No lock is held here: each operation takes the service lock itself.
 * ========================================================================== */
#define TA_STEP(label, call)                                                   \
    do {                                                                       \
        steps++;                                                               \
        ca_stdout_printf_n("[test_all] %2u. %s", (unsigned)steps, (label));    \
        if (!(call)) {                                                         \
            failed++;                                                          \
            ca_stdout_printf_n("[test_all]     ^^ FAILED: %s", (label));       \
        }                                                                      \
    } while (0)

static void cmd_test_all(void)
{
    u32 steps = 0U, failed = 0U;
    u32 t0 = ca_os_tick_ms();

    log_stop_for("test_all");

    ca_stdout_put_n("========================================================");
    ca_stdout_put_n("[test_all] EX58 full sequence (DESTRUCTIVE on the card)");
    ca_stdout_put_n("========================================================");

    /* 1) Raw driver first, before any file system exists: a card that fails
     *    here would make every FatFS error below meaningless. LBA 0 is safe
     *    precisely because fdisk/mkfs come next and rewrite it. */
    TA_STEP("sdtest 0 8   (raw write/read/verify)",  cmd_sdtest("0", "8"));
    TA_STEP("sdtest 0 16  (raw, larger burst)",      cmd_sdtest("0", "16"));

    /* 2) Volume: partition, format, mount. mount also starts the stdout
     *    logger, so everything printed from here lands in the log file too. */
    TA_STEP("fdisk 64     (partition table)",        ca_fat_fdisk(64U));
    TA_STEP("mkfs         (format FAT32)",           ca_fat_mkfs());
    TA_STEP("mount",                                 ca_fat_mount());
    TA_STEP("free         (space report)",           ca_fat_free());
    TA_STEP("pwd",                                   ca_fat_pwd());

    /* 3) Directories. */
    TA_STEP("mkdir tdir",                            ca_fat_mkdir("tdir"));
    TA_STEP("cd tdir",                               ca_fat_cd("tdir"));
    TA_STEP("pwd          (inside tdir)",            ca_fat_pwd());

    /* 4) Files, inside the new directory. */
    TA_STEP("touch t.txt",                           ca_fat_touch("t.txt"));
    TA_STEP("ls           (tdir, expect t.txt)",     ca_fat_ls(""));
    TA_STEP("cat t.txt",                             ca_fat_cat("t.txt"));
    TA_STEP("blob b.bin 64 K",                       cmd_blob("b.bin", "64", "K"));
    TA_STEP("ls           (expect t.txt + b.bin)",   ca_fat_ls(""));
    TA_STEP("free         (after 64 KB written)",    ca_fat_free());

    /* 5) Tear down what was created, in reverse order: rmdir only succeeds on
     *    an empty directory, so it doubles as a check that rm really removed
     *    both files. */
    TA_STEP("rm b.bin",                              ca_fat_rm("b.bin"));
    TA_STEP("rm t.txt",                              ca_fat_rm("t.txt"));
    TA_STEP("ls           (expect empty)",           ca_fat_ls(""));
    TA_STEP("cd ..",                                 ca_fat_cd(".."));
    TA_STEP("rmdir tdir   (fails if not empty)",     ca_fat_rmdir("tdir"));
    TA_STEP("ls           (root, expect no tdir)",   ca_fat_ls(""));

    /* 6) Close the volume and the log file. */
    TA_STEP("umount",                                ca_fat_umount());

    u32 ms = ca_os_tick_ms() - t0;
    ca_stdout_put_n("========================================================");
    if (failed == 0U) {
        ca_stdout_printf_n("[test_all] RESULT: PASS  (%u/%u steps, %u ms)",
                           (unsigned)steps, (unsigned)steps, (unsigned)ms);
    } else {
        ca_stdout_printf_n("[test_all] RESULT: FAIL  (%u/%u steps failed, %u ms)",
                           (unsigned)failed, (unsigned)steps, (unsigned)ms);
    }
    ca_stdout_put_n("========================================================");
}

/* ============================================================================
 * Fault injection: a pin the MCU drives, taken away in the middle of a transfer
 * --------------------------------------------------------------------------
 * inject <Pn> <delay_ms> <hold_ms>: after delay_ms the pin goes to input
 * (MODER = 00, CRL/CRH floating on F1) for hold_ms, then its mode is put back. AFR is not touched, so
 * the restore brings the alternate function back as it was. Armed here, it
 * fires while the next command runs (fat sdtest).
 *
 * For a pin the MCU drives only - SDIO CK (C12): the card stops on a missing
 * clock exactly as if it had been pulled out, still powered. Never D0-D3 or
 * CMD: the card drives those too, and an output there would fight it.
 * ========================================================================== */
typedef struct {
    GPIO_TypeDef *gpio;
    u32 pin;
    u32 delay_ms;
    u32 hold_ms;
} inject_t;

static inject_t s_inj;
static volatile bool s_inj_busy;

/* Pin to input, returns the previous mode field. Called in a critical section. */
static u32 inject_release(GPIO_TypeDef *gpio, u32 pin)
{
#if MCU_ID == 103
    /* F1: CRL/CRH, 4 bits per pin (CNF:MODE); 0100 = floating input */
    volatile u32 *cr = (pin < 8U) ? &gpio->CRL : &gpio->CRH;
    const u32 sh = (pin & 7U) * 4U;
    const u32 mode = (*cr >> sh) & 0xFU;
    *cr = (*cr & ~(0xFU << sh)) | (0x4U << sh);
#else
    const u32 sh = pin * 2U;
    const u32 mode = (gpio->MODER >> sh) & 3U;
    gpio->MODER &= ~(3U << sh);
#endif
    return mode;
}

static void inject_restore(GPIO_TypeDef *gpio, u32 pin, u32 mode)
{
#if MCU_ID == 103
    volatile u32 *cr = (pin < 8U) ? &gpio->CRL : &gpio->CRH;
    const u32 sh = (pin & 7U) * 4U;
    *cr = (*cr & ~(0xFU << sh)) | (mode << sh);
#else
    const u32 sh = pin * 2U;
    gpio->MODER = (gpio->MODER & ~(3U << sh)) | (mode << sh);
#endif
}

static u32 task_inject(void *arg)
{
    inject_t *in = (inject_t *)arg;

    ca_os_sleep(in->delay_ms);
    ca_enter_critical();
    const u32 mode = inject_release(in->gpio, in->pin);   /* input: the pin lets go */
    ca_exit_critical();

    const u32 t0 = ca_os_tick_ms();
    ca_os_sleep(in->hold_ms);

    ca_enter_critical();
    inject_restore(in->gpio, in->pin, mode);
    ca_exit_critical();
    ca_stdout_printf_n("[inject] pin released for %u ms, mode %u restored",
                       ca_os_tick_ms() - t0, mode);
    s_inj_busy = false;
    return 0;
}

/* "C12" -> port C, pin 12 */
static bool cmd_inject(const char *pin_s, const char *delay_s, const char *hold_s)
{
    u16 idx = 0;
    const char port = (char)(pin_s[0] & ~0x20);     /* upper case */
    const u32 pin   = (u32)ca_strtol_index(pin_s + 1, 10, &idx);
    if (port < 'A' || port > 'K' || pin > 15U || !delay_s[0] || !hold_s[0]) {
        ca_stdout_put_n("[inject] inject <Pn> <delay_ms> <hold_ms>  e.g. inject C12 100 500");
        return false;
    }
    if (s_inj_busy) { ca_stdout_put_n("[inject] busy"); return false; }

    idx = 0;
    s_inj.delay_ms = (u32)ca_strtol_index(delay_s, 10, &idx);
    idx = 0;
    s_inj.hold_ms  = (u32)ca_strtol_index(hold_s, 10, &idx);
    s_inj.gpio     = ca_gpio_get_io_port((ca_gpio_port_t)(port - 'A'));
    s_inj.pin      = pin;
    s_inj_busy     = true;
    if (ca_os_job("inject", CA_TH_LEVEL5, task_inject, &s_inj,
                  STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT) == CA_TASK_HANDLE_NONE) {
        s_inj_busy = false;
        ca_stdout_put_n("[inject] no task");
        return false;
    }
    ca_stdout_printf_n("[inject] armed: %c%u to input in %u ms for %u ms",
                       port, (unsigned)pin, (unsigned)s_inj.delay_ms, (unsigned)s_inj.hold_ms);
    return true;
}

/* ============================================================================
 * CLI — the example's own `fat` subcommands
 * --------------------------------------------------------------------------
 * Registered AFTER the service, so it sits ahead of it in the chain
 * (ca_cli_register pushes at the head): it answers sdtest/blob/test_all/log
 * and returns false on everything else, which then reaches the service node.
 * ========================================================================== */
static const char *next_tok(const char *p, char *out, u32 maxlen)
{
    p = ca_skip_sp(p);
    if (!p || !*p) { out[0] = '\0'; return p; }
    u32 i = 0;
    while (*p && *p != ' ' && *p != '\t' && i < maxlen - 1U)
        out[i++] = *p++;
    out[i] = '\0';
    return p;
}

static bool ex58_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        ca_stdout_put_n("fat       : blob <f> <n> <B|K|M> | sdtest <lba> [<n>] (raw, destructive)");
        ca_stdout_put_n("            test_all (DESTRUCTIVE) | log - stdout to <uptime>_cmd_log.log");
        ca_stdout_put_n("inject    : <Pn> <delay_ms> <hold_ms> - pin to input mid-transfer (C12 = SDIO CK)");
        return false;
    }

    if (ca_strncmp(p, "inject", 6) == 0 && (p[6] == '\0' || p[6] == ' ')) {
        char hold[12];
        const char *args = next_tok(ca_skip_sp(p + 6), g_ctx->a1, sizeof(g_ctx->a1));
        args = next_tok(args, g_ctx->a2, sizeof(g_ctx->a2));
        next_tok(args, hold, sizeof(hold));
        cmd_inject(g_ctx->a1, g_ctx->a2, hold);
        return true;
    }

    if (ca_strncmp(p, "fat", 3) != 0 || (p[3] != '\0' && p[3] != ' ')) return false;
    const char *args = next_tok(ca_skip_sp(p + 3), g_ctx->sub, sizeof(g_ctx->sub));
    const char *sub = g_ctx->sub;
    char *a1 = g_ctx->a1, *a2 = g_ctx->a2, *a3 = g_ctx->a3;

    if (ca_strcmp(sub, "test_all") == 0) { cmd_test_all(); return true; }

    if (ca_strcmp(sub, "sdtest") == 0) {
        args = next_tok(args, a1, sizeof(g_ctx->a1));
        next_tok(args, a2, sizeof(g_ctx->a2));
        if (!a1[0]) ca_stdout_put_n("[fat] sdtest: sdtest <lba> [<n>] (DESTRUCTIVE)");
        else        cmd_sdtest(a1, a2);
        return true;
    }

    if (ca_strcmp(sub, "blob") == 0) {
        args = next_tok(args, a1, sizeof(g_ctx->a1));
        args = next_tok(args, a2, sizeof(g_ctx->a2));
        next_tok(args, a3, sizeof(g_ctx->a3));
        if (!a1[0] || !a2[0]) ca_stdout_put_n("[fat] blob: blob <file> <n> <B|K|M>");
        else                  cmd_blob(a1, a2, a3[0] ? a3 : "B");
        return true;
    }

    /* Reads no file system state, so it does not take the lock - and must not,
     * since the point is to report what the lock made the logger drop. */
    if (ca_strcmp(sub, "log") == 0) {
        if (!g_ctx->logging) { ca_stdout_put_n("[log] not logging"); return true; }
        ca_stdout_printf_n("[log] written=%u lines, dropped=%u, sync every %u",
                           (unsigned)g_ctx->log_lines, (unsigned)g_ctx->log_lost,
                           (unsigned)LOG_SYNC_EVERY);
        ca_stdout_put_n("[log] a 'fat' command holds FatFS while it prints, so its own");
        ca_stdout_put_n("[log] output is dropped: the file grows from other tasks' lines.");
        return true;
    }

    return false;   /* a service command: let the next node have it */
}

static ca_cli_node_t s_ex58_cli_node = { ex58_cli_handler, NULL };

/* ============================================================================
 * Heartbeat task
 * ========================================================================== */
static u32 task_heartbeat(void *arg)
{
    (void)arg;
    ca_led_init();
    for (;;) {
        ca_led_toggle(1U);
        ca_os_sleep(500);
    }
    return 0;
}

/* ============================================================================
 * Configure
 * ========================================================================== */
void ca_os_configure(void)
{
    /* Example context is static: nothing to allocate, just clear it. */
    ca_memset(g_ctx, 0, sizeof(ex58_ctx_t));

    EVAL_DMA( ca_dma_init(); )   /* before SDIO: reserve DMA2 Stream3 */

    /* Bring the SDIO driver up here rather than on first use.
     *
     * ca_sdio_init() is what registers the "sdio" CLI node (sdio.c:89), and
     * it used to run only from `sdtest` - so on a freshly booted board
     * EN_CLI_SDIO=1 promised a command that was not in the help until
     * something happened to touch the card. A config flag that is on must
     * mean the command is there, always. The return value is deliberately
     * ignored: an absent card is a normal state, the CLI must still answer. */
    EVAL_SDIO( (void)ca_sdio_init(); )
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
    )
    /* After ca_cli_init (it registers the `fat` node), and BEFORE the
     * example's node, which must sit ahead of it in the chain. */
    ca_fat_service_init();
    ca_fat_service_set_event_cb(fat_event);
    EVAL_CLI(
        ca_cli_register(&s_ex58_cli_node);
        ca_cli_create_thread();
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}

/* ============================================================================
 * Main
 * ========================================================================== */
void ca_os_main(void)
{
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* EN_FAT && EN_FAT_SERVICE && EN_SDIO && EN_DMA && EN_RTC not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m58a, "=== Example 58 ===");
PRIVATE_STRING(m58b, "EN_FAT / EN_FAT_SERVICE / EN_SDIO / EN_DMA / EN_RTC disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m58a);
    PUT_N(&priv_m58b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_FAT && EN_FAT_SERVICE && EN_SDIO && EN_DMA && EN_RTC */
