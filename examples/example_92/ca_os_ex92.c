// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex92.c
 * @brief          : Example 92 — external SPI NOR flash on the QSPI/OCTOSPI
 ******************************************************************************
 * @details
 * Drives a W25Qxx-class serial NOR flash through the QSPI driver (QUADSPI on
 * F446/F7/H7, OCTOSPI1 on H5/U5), in single and in dual mode and through the
 * memory-mapped window at 0x90000000. The driver is interrupt driven: data on
 * the FIFO interrupt, erase/program waits on the controller's auto-polling.
 *
 * HARDWARE — a 6-pin module (VCC CS DO GND CLK DI), on the Zio connectors.
 * The POSITIONS are the Nucleo-144 Zio QSPI ones, the same on every board;
 * the MCU pins behind them differ and come from the driver (pinmap 0):
 *
 *   module 1 VCC  -> CN8-7   3V3   (CN10 has no 3V3 - pin 1 is AVDD)
 *   module 2 CS   -> CN10-13 NCS   F446/F7: PB6  H7/H5: PG6  U575: PA2
 *   module DI     -> CN10-23 IO0   F446/F7/H7/H5: PD11       U575: PE12
 *   module 4 GND  -> CN10-17 GND
 *   module 5 CLK  -> CN10-15 CLK   F446/F7/H7/H5: PB2        U575: PB10
 *   module DO     -> CN10-21 IO1   F446/F7/H7/H5: PD12       U575: PB0
 *
 * DO/DI are the flash's data OUT/IN (easily read as D0/D1): DI is IO0, DO is
 * IO1. "e92 wire" also probes them swapped and says so.
 *
 * Only DI/DO are wired, so the module runs single (1-1-1) or dual output
 * (1-1-2, 0x3B) - never quad. WP and HOLD must be pulled up on the module:
 * a floating HOLD freezes the flash. "e92 wire" prints this table with the
 * MCU pins of the board under test.
 *
 * NOTHING RUNS BY ITSELF. The board is idle at boot; every test is ONE
 * deterministic pass ending with a verdict the bench can parse:
 *
 *   e92 wire    module present? JEDEC ID read on PLAIN GPIO (no QSPI), so a
 *               FAIL accuses the wires, not the driver -> WIRE RESULT
 *   e92 start   all: id, init, erase, write, read single, read dual, and
 *               the same data through the memory-mapped window (mmap)
 *   e92 write   erase + write the test pattern, read it back -> RUN RESULT
 *   e92 read    read the pattern back (single, dual, mmap) WITHOUT writing:
 *               after a power cycle it proves the data was retained
 *   e92 bench   read throughput, 1 MB: indirect (IRQ) vs memory-mapped
 *   e92 sweep   clock sweep, 7.8 to 125 MHz: write+single and dual at each
 *               divider, one line per clock and the fastest that held
 *   e92 clk     <MHz> clock target of the next tests (default 8)
 *   e92 shift   auto|on|off - read sample shift of the next tests
 *   e92 dump    [addr hex] 64 bytes, single mode
 *   e92 info    last ID, clock, counters
 *
 * The test area is one 4 KB sector at EX92_SECTOR; the pattern starts at
 * offset EX92_OFF so that it crosses two 256-byte page boundaries and the
 * driver's page split is exercised too.
 ******************************************************************************
 */

#include "caos.h"

#if EN_QSPI && MCU_HAS_QSPI

#define EX92_PINMAP   0U           /* Zio CN10 on every Nucleo-144          */
#define EX92_SECTOR   0x001000U    /* second 4 KB sector: exists on any size */
#define EX92_OFF      0x80U        /* crosses two page boundaries           */
#define EX92_LEN      512U
#define EX92_SCK_HZ   8000000U     /* default: flying wires, keep it low     */

/* Kernel clock of the QSPI/OCTOSPI: HCLK, which is the core clock except on
 * H7, where the QUADSPI runs from rcc_hclk3 = core / 2. */
#if (MCU_ID == 753) || (MCU_ID == 747)
#define EX92_KER_HZ   (SystemCoreClock / 2U)
#else
#define EX92_KER_HZ   (SystemCoreClock)
#endif

/* Module pin, its Zio position and the driver signal behind it. The data
 * lines go by FUNCTION, not by header number: modules print DO/DI (data out /
 * in of the flash), easily read as D0/D1, and the header order varies. */
typedef struct { const char *mod; const char *pos; i8 sig; } ex92_wire_t;
static const ex92_wire_t s_wire[] = {
    { "1 VCC", "CN8-7  ", -1 },
    { "2 CS ", "CN10-13", (i8)CA_QSPI_PIN_NCS },
    { "DI   ", "CN10-23", (i8)CA_QSPI_PIN_IO0 },   /* data IN of the flash  */
    { "4 GND", "CN10-17", -1 },
    { "5 CLK", "CN10-15", (i8)CA_QSPI_PIN_CLK },
    { "DO   ", "CN10-21", (i8)CA_QSPI_PIN_IO1 },   /* data OUT of the flash */
};

typedef enum { REQ_NONE = 0, REQ_WIRE, REQ_ALL, REQ_WRITE, REQ_READ, REQ_DUMP,
               REQ_SWEEP, REQ_BENCH } ex92_req_t;

static volatile ex92_req_t s_req;
static volatile u32        s_dump_addr;
static u8  s_id[3];
static u32 s_clk_div;
static volatile u32 s_sck_hz = EX92_SCK_HZ;   /* "e92 clk <MHz>" */
static i32 s_div_force = -1;                  /* sweep: divider, not Hz */
static bool s_quiet;                          /* sweep: no STEP lines   */
static volatile u8 s_sshift = CA_QSPI_SSHIFT_AUTO;   /* "e92 shift" */
static u32 s_runs_ok, s_runs_fail;
static u8  s_wbuf[EX92_LEN];
static u8  s_rbuf[EX92_LEN];

/* ---- plain-GPIO SPI (mode 0): the wire test owns the pins, not the QSPI ---- */
static ca_gpio_port_t s_p[CA_QSPI_PIN_COUNT];
static ca_gpio_pin_t  s_n[CA_QSPI_PIN_COUNT];

/* Which line the MCU drives and which it reads: IO0/IO1 normally, the other
 * way round when wire_test() probes for a module wired with DO/DI swapped. */
static ca_qspi_pin_t s_mosi = CA_QSPI_PIN_IO0;
static ca_qspi_pin_t s_miso = CA_QSPI_PIN_IO1;

static void bb_delay(void) { for (volatile u32 d = 0U; d < 50U; d++) { } }

static void bb_out(ca_qspi_pin_t s, u8 v) { ca_gpio_write(s_p[s], s_n[s], v); }

static u8 bb_xfer(u8 out)
{
    u8 in = 0U;
    for (u8 b = 0U; b < 8U; b++) {
        bb_out(s_mosi, (u8)((out >> 7) & 1U));
        out = (u8)(out << 1);
        bb_delay();
        bb_out(CA_QSPI_PIN_CLK, 1U);
        bb_delay();
        in = (u8)((in << 1) | (ca_gpio_read(s_p[s_miso], s_n[s_miso]) & 1U));
        bb_out(CA_QSPI_PIN_CLK, 0U);
    }
    return in;
}

/* JEDEC ID (0x9F) on plain GPIO, after a release from power-down (0xAB).
 * D1 has a pull-down: a missing wire reads a stable 00, not noise. The pins
 * stay GPIO afterwards; the next QSPI init gives them back to the AF. */
static void bb_jedec(u8 id[3], bool swapped)
{
    s_mosi = swapped ? CA_QSPI_PIN_IO1 : CA_QSPI_PIN_IO0;
    s_miso = swapped ? CA_QSPI_PIN_IO0 : CA_QSPI_PIN_IO1;
    for (u8 s = 0U; s < (u8)CA_QSPI_PIN_COUNT; s++)
        (void)ca_qspi_get_pin(EX92_PINMAP, (ca_qspi_pin_t)s, &s_p[s], &s_n[s]);

    const ca_qspi_pin_t outs[] = { CA_QSPI_PIN_NCS, CA_QSPI_PIN_CLK, s_mosi };
    for (u8 i = 0U; i < 3U; i++)
        ca_gpio_configure(s_p[outs[i]], s_n[outs[i]], CA_GPIO_MODE_OUTPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_configure(s_p[s_miso], s_n[s_miso], CA_GPIO_MODE_INPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

    bb_out(CA_QSPI_PIN_NCS, 1U);
    bb_out(CA_QSPI_PIN_CLK, 0U);
    ca_os_sleep(1);

    bb_out(CA_QSPI_PIN_NCS, 0U);
    (void)bb_xfer(0xABU);                 /* release from power-down */
    bb_out(CA_QSPI_PIN_NCS, 1U);
    ca_os_sleep(1);                       /* tRES1 = 3 us */

    bb_out(CA_QSPI_PIN_NCS, 0U);
    (void)bb_xfer(0x9FU);
    for (u8 i = 0U; i < 3U; i++) id[i] = bb_xfer(0x00U);
    bb_out(CA_QSPI_PIN_NCS, 1U);
}

static bool id_valid(const u8 id[3])
{
    return (id[0] != 0x00U) && (id[0] != 0xFFU);
}

static void pin_name(ca_qspi_pin_t s, char out[6])
{
    ca_gpio_port_t p; ca_gpio_pin_t n;
    if (!ca_qspi_get_pin(EX92_PINMAP, s, &p, &n)) { ca_strcpy(out, "?"); return; }
    ca_snprintf(out, 6, "P%c%u", (char)('A' + (u32)p), (u32)n);
}

/* ---- tests ---- */
static void wire_test(void)
{
    ca_stdout_printf_n("e92 wire: module -> Zio position (MCU_ID %u, pinmap %u)",
                       (u32)MCU_ID, (u32)EX92_PINMAP);
    for (u32 i = 0U; i < (sizeof(s_wire) / sizeof(s_wire[0])); i++) {
        char pn[6];
        if (s_wire[i].sig < 0) ca_strcpy(pn, (i == 0U) ? "3V3" : "GND");
        else                   pin_name((ca_qspi_pin_t)s_wire[i].sig, pn);
        ca_stdout_printf_n("  module %s -> %s  %s", s_wire[i].mod, s_wire[i].pos, pn);
    }

    u8 a[3], b[3];
    bb_jedec(a, false);
    bb_jedec(b, false);
    const bool same = (a[0] == b[0]) && (a[1] == b[1]) && (a[2] == b[2]);
    const bool ok   = same && id_valid(a);
    ca_memcpy(s_id, a, 3U);

    /* No answer: try the data lines the other way round. W25Q modules print
     * DO/DI (data OUT/IN of the flash), easily read as D0/D1 - and DO goes
     * to IO1, DI to IO0. Swapped, the MCU drives the line the flash answers
     * on and reads one nobody drives: 00 00 00, as if nothing were there. */
    if (!ok) {
        u8 s[3];
        bb_jedec(s, true);
        if (id_valid(s)) {
            ca_stdout_printf_n("  swapped   : %02X %02X %02X  the flash answers with D0/D1"
                               " EXCHANGED", s[0], s[1], s[2]);
            ca_stdout_put_n("              module DO (data out) -> CN10-21 (IO1),"
                            " DI (data in) -> CN10-23 (IO0)");
        }
    }

    ca_stdout_printf_n("  JEDEC ID  : %02X %02X %02X  %s", a[0], a[1], a[2],
                       ok ? "OK (flash answers)" :
                       (!same)           ? "FAIL (unstable - loose wire?)" :
                       (a[0] == 0x00U)   ? "FAIL (no answer: VCC/GND/CS/CLK/D0/D1)" :
                                           "FAIL (D1 stuck high)");
    if (ok && a[2] >= 0x10U && a[2] <= 0x1FU)
        ca_stdout_printf_n("  capacity  : %u KB", (u32)(1UL << (a[2] - 10U)));
    ca_stdout_printf_n("  WIRE RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
}

static void fill_pattern(u8 *buf)
{
    u32 x = 0x92A5C3E1U;                  /* fixed seed: same data every run */
    for (u32 i = 0U; i < EX92_LEN; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        buf[i] = (u8)x;
    }
}

static bool step(const char *name, bool ok, const char *why)
{
    if (s_quiet) return ok;
    ca_stdout_printf_n("  STEP %-6s : %s%s%s", name, ok ? "OK" : "FAIL",
                       (ok || !why) ? "" : "  ", (ok || !why) ? "" : why);
    return ok;
}

static bool qspi_open(bool dual)
{
    u32 div;
    if (s_div_force >= 0) {
        div = (u32)s_div_force;
    } else {
        div = (EX92_KER_HZ + s_sck_hz - 1U) / s_sck_hz;
        div = (div > 0U) ? div - 1U : 0U;
    }
    if (div > 255U) div = 255U;
    s_clk_div = div;

    ca_qspi_cfg_t cfg = {
        .clk_div       = (u8)div,
        .flash_size    = (s_id[2] >= 0x10U && s_id[2] <= 0x1FU) ? (u8)(s_id[2] - 1U) : 21U,
        .addr_bytes    = 3U,
        .dummy_cycles  = 8U,
        .data_lines    = dual ? 2U : 1U,
        .cmd_read      = dual ? 0x3BU : 0x0BU,   /* dual output / fast read */
        .cmd_page_prog = 0x02U,
        .cmd_sector_er = 0x20U,
        .cmd_wren      = 0x06U,
        .cmd_rdsr      = 0x05U,
        .pinmap        = EX92_PINMAP,
        .addr_lines    = 1U,
        .sample_shift  = s_sshift,
    };
    return ca_qspi_init(&cfg);
}

static bool compare(u32 *bad)
{
    for (u32 i = 0U; i < EX92_LEN; i++)
        if (s_rbuf[i] != s_wbuf[i]) { *bad = i; return false; }
    return true;
}

static bool read_check(bool dual)
{
    char why[48] = "";
    u32 bad = 0U;
    if (!step(dual ? "init2" : "init1", qspi_open(dual), "ca_qspi_init")) return false;
    ca_memset(s_rbuf, 0, EX92_LEN);
    if (!step(dual ? "read2" : "read1",
              ca_qspi_read(EX92_SECTOR + EX92_OFF, s_rbuf, EX92_LEN), "transfer"))
        return false;
    const bool ok = compare(&bad);
    if (!ok) ca_snprintf(why, sizeof(why), "@%u wrote %02X read %02X",
                         bad, s_wbuf[bad], s_rbuf[bad]);
    return step(dual ? "dual" : "single", ok, why);
}

/* The same pattern through the memory-mapped window: no command, no call -
 * the flash read like memory at CA_QSPI_MMAP_BASE. The driver leaves the
 * mode by itself on the next indirect operation. */
static bool mmap_check(void)
{
    char why[48] = "";
    const u32 base = ca_qspi_mmap_enable();
    if (!step("mmapon", base != 0U, "ca_qspi_mmap_enable")) return false;
    const volatile u8 *m = (const volatile u8 *)(base + EX92_SECTOR + EX92_OFF);
    u32 bad = EX92_LEN;
    for (u32 i = 0U; i < EX92_LEN; i++)
        if (m[i] != s_wbuf[i]) { bad = i; break; }
    if (bad != EX92_LEN)
        ca_snprintf(why, sizeof(why), "@%u wrote %02X read %02X",
                    bad, s_wbuf[bad], (u32)m[bad]);
    ca_qspi_mmap_disable();
    return step("mmap", bad == EX92_LEN, why);
}

static bool write_check(void)
{
    char why[48] = "";
    u32 bad = 0U;
    if (!step("init1", qspi_open(false), "ca_qspi_init")) return false;
    if (!step("erase", ca_qspi_erase_sector(EX92_SECTOR), "erase/WIP")) return false;

    ca_memset(s_rbuf, 0, EX92_LEN);
    bool blank = ca_qspi_read(EX92_SECTOR + EX92_OFF, s_rbuf, EX92_LEN);
    for (u32 i = 0U; blank && i < EX92_LEN; i++)
        if (s_rbuf[i] != 0xFFU) { bad = i; blank = false; }
    if (!blank) ca_snprintf(why, sizeof(why), "@%u read %02X", bad, s_rbuf[bad]);
    if (!step("blank", blank, why)) return false;

    if (!step("write", ca_qspi_write(EX92_SECTOR + EX92_OFF, s_wbuf, EX92_LEN),
              "page program/WIP"))
        return false;
    return read_check(false);
}

static void verdict(bool ok)
{
    if (ok) s_runs_ok++; else s_runs_fail++;
    ca_stdout_printf_n("  RUN RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
}

/* The ID is read before every run: it sizes DEVSIZE, and a run on a module
 * that is not there should say so first instead of failing an erase. */
static bool id_step(void)
{
    bb_jedec(s_id, false);
    char why[24] = "";
    ca_snprintf(why, sizeof(why), "ID %02X %02X %02X", s_id[0], s_id[1], s_id[2]);
    return step("id", id_valid(s_id), why);
}

/* Read throughput at the current clock and mode, indirect (interrupt
 * driven, 512-byte calls) against memory-mapped (memcpy from the window).
 * Information, not a verdict: the numbers depend on the clock and lines. */
static void bench(void)
{
    const u32 total = 1024U * 1024U;
    if (!id_step() || !qspi_open(false)) { verdict(false); return; }
    u32 t0 = ca_os_tick_ms();
    bool ok = true;
    for (u32 a = 0U; ok && a < total; a += EX92_LEN)
        ok = ca_qspi_read(a, s_rbuf, EX92_LEN);
    const u32 t_ind = ca_os_tick_ms() - t0;
    const u32 base = ok ? ca_qspi_mmap_enable() : 0U;
    t0 = ca_os_tick_ms();
    for (u32 a = 0U; base && a < total; a += EX92_LEN)
        ca_memcpy(s_rbuf, (const void *)(base + a), EX92_LEN);
    const u32 t_map = ca_os_tick_ms() - t0;
    ca_qspi_mmap_disable();
    ca_stdout_printf_n("  BENCH clk_div %u (%u kHz) single, 1 MB:", s_clk_div,
                       (u32)(EX92_KER_HZ / 1000U) / (s_clk_div + 1U));
    ca_stdout_printf_n("  BENCH indirect : %u ms  %u KB/s", t_ind,
                       t_ind ? (1024U * 1000U) / t_ind : 0U);
    ca_stdout_printf_n("  BENCH mmap     : %u ms  %u KB/s", t_map,
                       t_map ? (1024U * 1000U) / t_map : 0U);
    verdict(ok && base != 0U);
}

/* Clock sweep: for each divider, from slow to fast, the whole write path at
 * that clock (erase, blank, page program, single read-back) and a dual read,
 * once with the sample shift OFF and once ON - so the line shows where the
 * half-cycle shift starts to matter, which is what the driver's AUTO
 * threshold (CA_QSPI_SSHIFT_AUTO_HZ) bets on. The limit is information, not
 * a failure: the verdict is the slowest clock, shift off. Dividers stop at
 * 1: div 0 is the kernel clock itself, past the 133 MHz of a W25Q. */
static bool sweep_one(u8 shift)
{
    s_sshift = shift;
    const bool w = write_check();
    return w && read_check(true);
}

static void sweep(void)
{
    static const u8 divs[] = { 31U, 15U, 9U, 7U, 5U, 4U, 3U, 2U, 1U };
    const u8 keep = s_sshift;
    u32 best_off = 0U, best_on = 0U;
    bool first_ok = false;
    if (!id_step()) { verdict(false); return; }
    s_quiet = true;
    for (u32 i = 0U; i < sizeof(divs); i++) {
        s_div_force = (i32)divs[i];
        const u32 khz = (EX92_KER_HZ / 1000U) / ((u32)divs[i] + 1U);
        const bool off = sweep_one(CA_QSPI_SSHIFT_OFF);
        const bool on  = sweep_one(CA_QSPI_SSHIFT_ON);
        ca_stdout_printf_n("  SWEEP %3u.%u MHz (div %2u) : shift off %s  shift on %s%s",
                           khz / 1000U, (khz % 1000U) / 100U, (u32)divs[i],
                           off ? "OK  " : "FAIL", on ? "OK  " : "FAIL",
                           (khz * 1000U > CA_QSPI_SSHIFT_AUTO_HZ) ? "  (auto: on)" : "  (auto: off)");
        if (i == 0U) first_ok = off;
        if (off) best_off = khz;
        if (on)  best_on  = khz;
    }
    s_quiet = false;
    s_div_force = -1;
    s_sshift = keep;
    ca_stdout_printf_n("  SWEEP max : shift off %u.%u MHz, shift on %u.%u MHz"
                       " (write + single + dual all passing)",
                       best_off / 1000U, (best_off % 1000U) / 100U,
                       best_on / 1000U, (best_on % 1000U) / 100U);
    verdict(first_ok);
}

static u32 task_ex92(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    fill_pattern(s_wbuf);
    ca_stdout_put_n("\r\n=== Example 92: SPI NOR flash on QSPI ===");
    ca_stdout_put_n("ex92: idle - \"e92 wire\" for the wiring and the module check,"
                    " \"e92 start\" to run every test");

    while (1) {
        while (s_req == REQ_NONE) ca_os_sleep(50);
        const ex92_req_t r = s_req;

        switch (r) {
        case REQ_WIRE:
            wire_test();
            break;
        case REQ_ALL: {
            bool ok = id_step() && write_check() && read_check(true) && mmap_check();
            verdict(ok);
            break;
        }
        case REQ_WRITE:
            verdict(id_step() && write_check());
            break;
        case REQ_READ:
            verdict(id_step() && read_check(false) && read_check(true) && mmap_check());
            break;
        case REQ_SWEEP:
            sweep();
            break;
        case REQ_BENCH:
            bench();
            break;
        case REQ_DUMP:
            if (id_step() && qspi_open(false) &&
                ca_qspi_read(s_dump_addr, s_rbuf, 64U)) {
                ca_stdout_printf_n("e92 dump @ 0x%06X:", s_dump_addr);
                ca_hexdump(s_rbuf, 64U);
            } else {
                ca_stdout_put_n("e92 dump: read failed");
            }
            break;
        default:
            break;
        }
        s_req = REQ_NONE;
    }
    return 0;
}

/* ---- CLI "e92" ---- */
#if ENABLE_CLI
#define EX92_HELP "e92       : start | write | read | wire | sweep | bench | clk <MHz> | shift auto|on|off | dump [addr hex] | info"

static bool ex92_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N(EX92_HELP); return false; }
    if (ca_strncmp(p, "e92", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("id      : %02X %02X %02X   clk_div %u -> %u kHz (kernel %u Hz)",
                     s_id[0], s_id[1], s_id[2], s_clk_div,
                     (u32)(EX92_KER_HZ / 1000U) / (s_clk_div + 1U), (u32)EX92_KER_HZ);
        CLI_PRINTF_N("shift   : %s   (auto: on above %u MHz)",
                     (s_sshift == CA_QSPI_SSHIFT_ON) ? "on" :
                     (s_sshift == CA_QSPI_SSHIFT_OFF) ? "off" : "auto",
                     (u32)(CA_QSPI_SSHIFT_AUTO_HZ / 1000000U));
        CLI_PRINTF_N("area    : 0x%06X + %u B   runs ok %u fail %u",
                     (u32)(EX92_SECTOR + EX92_OFF), (u32)EX92_LEN, s_runs_ok, s_runs_fail);
        return true;
    }

    if (ca_strncmp(p, "clk", 3) == 0) {
        p = ca_skip_sp(p + 3);
        const u32 mhz = (u32)ca_strtol(p, 10);
        if (mhz == 0U || mhz > 133U) { CLI_PUT_N("e92: clk <1..133 MHz>"); return true; }
        s_sck_hz = mhz * 1000000U;
        CLI_PRINTF_N("e92: clock target %u MHz from the next test", mhz);
        return true;
    }

    if (ca_strncmp(p, "shift", 5) == 0) {
        p = ca_skip_sp(p + 5);
        if      (ca_strncmp(p, "auto", 4) == 0) s_sshift = CA_QSPI_SSHIFT_AUTO;
        else if (ca_strncmp(p, "on",   2) == 0) s_sshift = CA_QSPI_SSHIFT_ON;
        else if (ca_strncmp(p, "off",  3) == 0) s_sshift = CA_QSPI_SSHIFT_OFF;
        else { CLI_PUT_N("e92: shift auto|on|off"); return true; }
        CLI_PUT_N("e92: sample shift set, from the next test");
        return true;
    }

    ex92_req_t r = REQ_NONE;
    if      (ca_strncmp(p, "wire",  4) == 0) r = REQ_WIRE;
    else if (ca_strncmp(p, "start", 5) == 0) r = REQ_ALL;
    else if (ca_strncmp(p, "write", 5) == 0) r = REQ_WRITE;
    else if (ca_strncmp(p, "read",  4) == 0) r = REQ_READ;
    else if (ca_strncmp(p, "sweep", 5) == 0) r = REQ_SWEEP;
    else if (ca_strncmp(p, "bench", 5) == 0) r = REQ_BENCH;
    else if (ca_strncmp(p, "dump",  4) == 0) {
        p = ca_skip_sp(p + 4);
        s_dump_addr = (*p != '\0') ? (u32)ca_strtol(p, 16) : (EX92_SECTOR + EX92_OFF);
        r = REQ_DUMP;
    }
    if (r == REQ_NONE) { CLI_PUT_N(EX92_HELP); return true; }

    if (s_req != REQ_NONE) { CLI_PUT_N("e92: busy - wait for the verdict"); return true; }
    s_req = r;
    return true;
}

static ca_cli_node_t s_ex92_cli_node = { ex92_cli_handler, NULL };
#endif

void ca_os_configure(void)
{
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    EVAL_USB_CDC_STD( ca_usb_dev_mng_cdc_cli(); )
    EVAL_CLI( ca_cli_register(&s_ex92_cli_node); )
}

void ca_os_main(void)
{
    ca_os_job("ex92", CA_TH_LEVEL4, task_ex92, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* !(EN_QSPI && MCU_HAS_QSPI) */

PRIVATE_STRING(m92a, "=== Example 92 ===");
PRIVATE_STRING(m92b, "no QUADSPI/OCTOSPI on this part (MCU_HAS_QSPI 0)");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m92a);
    PUT_N(&priv_m92b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_QSPI && MCU_HAS_QSPI */
