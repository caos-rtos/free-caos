// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex64.c
 * @brief          : Example 64 — SPI master in DMA, loopback on a single board
 ******************************************************************************
 * @details
 * Demonstrates the DMA mode of the SPI driver (EN_SPI_DMA): SYNCHRONOUS
 * full-duplex transfer with a known size a priori. The master clocks
 * exactly len bytes; the two DMA channels (RX P2M + TX M2P) are armed,
 * the transfer starts and the task blocks until completion — no active
 * waiting or per-byte interrupts.
 *
 * HARDWARE — 2-wire loopback on the master only (no slave):
 *   jumper MOSI ──── MISO   (the transmitted byte returns identical)
 *
 *   SPI1 default per family (see spi.h):
 *     PA7 (MOSI) ──── PA6 (MISO)      SCK PA5 free
 *
 *   NUCLEO-U575ZI-Q: no Ethernet mounted → PA5/PA6/PA7 free, using the
 *   DEFAULT mapping (.alternate_gpio = false). Loopback on ZIO CN7
 *   (Arduino), pins REACHABLE on this board (unlike the H563):
 *     U575:   PA7 (MOSI, D11, CN7 pin 14) ──── PA6 (MISO, D12, CN7 pin 12)
 *             SCK PA5 (D13, CN7 pin 10) not needed in 2-wire loopback.
 *             UM2861 Table 15. Verified working ✅ HW 20/07.
 *
 *   NUCLEO-H753ZI: Ethernet is fitted, so PA7 is RMII_CRS_DV and driven by
 *   the PHY (SB31 ON from the factory; UM2407 Table 14 says those pins "must
 *   not be used on the ST morpho or the ST Zio connectors"). The loopback
 *   therefore uses the alternative mapping, like the H563:
 *     H753:   PB5 (MOSI, CN7 pin 13) ──── PB4 (MISO, CN7 pin 19)
 *             The morpho brings out the same two nets on CN12-29/CN12-27;
 *             both wirings verified working ✅ HW 06/09.
 *
 *   NUCLEO-144 with Ethernet mounted (H563ZI): PA7 = RMII_CRS_DV, driven by
 *   the PHY → NOT usable as MOSI. On those boards, open the SPI with
 *   .alternate_gpio = true and the loopback becomes:
 *     H563ZI:  PB5 (MOSI, CN12 pin 29) ──── PB4 (MISO, CN12 pin 27)
 *              Use MORPHO CN12: verified working on MB1404.
 *              DO NOT use CN7: PB5 is not reachable there. Pin 13 (D22)
 *              passes through SB17, OFF by default; pin 14 (D11) does not carry
 *              the signal despite UM3115 Table 21 listing it as PB5.
 *
 * The task tries various sizes (1..256 bytes, pseudo-random pattern) and
 * verifies that rx == tx byte by byte. Counters exposed via CLI "e64 info".
 *
 * NOTHING RUNS BY ITSELF. The board is idle at boot; "e64 start" runs ONE
 * pass over every size, 1 to 256 bytes, and ends with a verdict:
 *
 *   RUN RESULT : OK  (1/1)          every transfer returned byte for byte
 *   RUN RESULT : FAIL  (0/1)        at least one did not
 *
 * then it goes back to idle. It used to cycle for ever from boot, printing a
 * line per transfer: there was no quiet moment to read any other command's
 * reply in - `wire` in particular, whose verdict competed with the traffic
 * (on H563 it never appeared at all) - and no notion of "the run succeeded",
 * only counters that never stopped moving. A pass is a finite, repeatable
 * experiment, which is what a bench can judge.
 *
 *   e64 start | info | regs | buf | wire
 *
 * REQUIREMENTS os_config.h:
 *   EN_SPI_DMA 1   (exclusive with EN_SPI; requires EN_DMA)
 *
 * NOTE H7xx: DMA buffers must not be in DTCM. On H753/H747 .bss is
 * mapped in AXI-SRAM (0x24000000), reachable by DMA1/DMA2 — the static
 * buffers below are therefore valid without dedicated sections.
 ******************************************************************************
 */

#include "caos.h"

#if EN_SPI_DMA && EN_DMA

#include "spi.h"

#define EX64_SPI     1U               /* SPI1 master */
#define EX64_BR      CA_SPI_BR_DIV64     /* moderate clock: reliable loopback */
#define EX64_MAXLEN  256U

/* Boards with the Ethernet PHY fitted drive PA7 themselves.
 *
 * PA7 is RMII_CRS_DV, and on those boards the solder bridge that ties it to
 * the PHY is ON from the factory - SB38 on the H563ZI, SB31 on the H753ZI,
 * whose UM2407 Table 14 states the case plainly: "These pins are used as RMII
 * signals and connected to Ethernet PHY. These pins must not be used on the
 * ST morpho or the ST Zio connectors."
 *
 * So the pin cannot serve as MOSI there, and the loopback moves to the
 * alternative mapping PB3/PB4/PB5. The condition read MCU_ID == 563 only,
 * which left the H753 on PA7/PA6: `wire` passed (the jumper really is there,
 * and continuity is all it measures) while every DMA transfer came back
 * rx=00, because the PHY was driving the line the SPI was trying to read.
 * Measured on H753: 19 of 19 transfers corrupted with the wire fitted and
 * verified. The U575 has no Ethernet mounted, so its default pins stay free.
 *
 * The F767 belongs on this list for the same reason and was missing from it:
 * the NUCLEO-F767ZI carries the PHY too, and PA7 is RMII_CRS_DV there - it is
 * in the very pin list ex33..ex45 program, all of them green on this board. It
 * would have repeated the H753 story exactly: a jumper that passes continuity
 * and every transfer coming back wrong, because the PHY drives the line. */
#if (MCU_ID == 563) || (MCU_ID == 753) || (MCU_ID == 747) || (MCU_ID == 767)
#define EX64_ALT_GPIO  true
#else
#define EX64_ALT_GPIO  false
#endif

/* Loopback pins, taken from the mapping this example actually opens - SPI1 uses
 * the same pins on every family (ca_gpio_hw_spi_init): PA5/PA6/PA7 by default,
 * PB3/PB4/PB5 on the alternate map. Keeping them here means "wire" tests the
 * pair the driver programmed, not a pair written down in a comment. */
#if EX64_ALT_GPIO
#define EX64_MOSI_PORT  CA_GPIO_PORT_B
#define EX64_MOSI_PIN   CA_GPIO_PIN_5
#define EX64_MISO_PORT  CA_GPIO_PORT_B
#define EX64_MISO_PIN   CA_GPIO_PIN_4
/* Same pins, DIFFERENT connector position per board - the pin is a fact of the
 * MCU, the position a fact of the PCB, and printing the wrong one sends the
 * operator to a header that does not carry the signal.
 *
 * H753 (UM2407 Table 18): CN7-13/CN7-19 on the Zio, which are the same nets as
 * CN12-29/CN12-27 on the morpho - either works, both verified on H753. The Zio
 * calls them SPI_B_MOSI/MISO after SPI3 while this example opens SPI1; a label
 * describes one possible use of a pin, it does not reserve it.
 *
 * H563 (UM3115, board MB1404): CN7 is NOT usable. Pin 13 (D22) goes through
 * SB17, open from the factory, and pin 14 (D11) does not carry PB5 despite the
 * table listing it. Use the morpho, as the header of this file says. */
#if (MCU_ID == 563)
#define EX64_WIRE_TXT   "PB5 (MOSI, CN12-29) ---- PB4 (MISO, CN12-27)"
#else
#define EX64_WIRE_TXT   "PB5 (MOSI, CN7-13) ---- PB4 (MISO, CN7-19)"
#endif
#else
#define EX64_MOSI_PORT  CA_GPIO_PORT_A
#define EX64_MOSI_PIN   CA_GPIO_PIN_7
#define EX64_MISO_PORT  CA_GPIO_PORT_A
#define EX64_MISO_PIN   CA_GPIO_PIN_6
/* CN12, the morpho - NOT CN7.
 *
 * UM2407 Table 14: CN7 pin 14 (D11) reaches PB5 or PA7 depending on the
 * SB33/SB35 pair, and the factory state is OFF,ON = PB5. So the label read
 * "PA7 ... CN7-14", a position that on an untouched board carries a different
 * pin entirely, and anyone wiring what it said would jumper PB5 to PA6 and be
 * told the wire was missing. The morpho brings both pins out unconditionally
 * (Table 22): PA7 on CN12-15, PA6 on CN12-13. */
#define EX64_WIRE_TXT   "PA7 (MOSI, CN12-15) ---- PA6 (MISO, CN12-13)"
#endif

/* ---- DMA buffers (AXI-SRAM on H7, not DTCM) ---- */
static u8 s_tx[EX64_MAXLEN] __attribute__((aligned(4)));
static u8 s_rx[EX64_MAXLEN] __attribute__((aligned(4)));

/* ---- statistics ---- */
static volatile u32 s_cnt_ok;       /* transfers verified OK      */
static volatile u32 s_cnt_err;      /* transfers with mismatch       */
static volatile u32 s_last_len;     /* last tested size        */
static volatile u32 s_last_errs;    /* erroneous bytes in last transfer */
static volatile u32 s_err_idx;      /* index of first erroneous byte         */
static volatile u8  s_err_tx;       /* tx[s_err_idx]                    */
static volatile u8  s_err_rx;       /* rx[s_err_idx]                    */

/* PRNG xorshift32 — repeatable pattern for the payload */
static u32 s_seed = 0x1234ABCDU;
static u8 prng_byte(void)
{
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return (u8)(s_seed & 0xFFU);
}

/* ---- loopback task ---- */
/* The loop is IDLE at boot; "e64 start" runs one pass and it parks again.
 *
 * It used to start on its own and print a line per transfer for ever, which
 * made every other command unreadable: `wire` writes levels on the very pins
 * the loop drives through SPI+DMA, and its verdict arrived buried in traffic
 * (on H563 it did not arrive at all). An example that talks non-stop cannot
 * be driven by a bench either - there is no quiet moment to read a reply in.
 * So: one pass per request, and silence until asked.
 *
 * s_loop_idle is the loop's ANSWER to s_loop_stop: the request alone says
 * nothing about when the current transfer ends, and ca_spi_dma_xfer() is
 * blocking. A caller that needs the pins waits for the confirmation. */
static volatile bool s_loop_stop = true;   /* idle until "e64 start" */
static volatile bool s_loop_idle;

/* Ask the loop to park and wait until it says it has. Returns false if it did
 * not stop in time - the caller then leaves the pins alone rather than
 * fighting the SPI for them. */
static bool loop_park(void)
{
    s_loop_stop = true;
    for (u32 i = 0U; i < 40U; i++) {       /* up to ~2 s */
        if (s_loop_idle) return true;
        ca_os_sleep(50);
    }
    return false;
}

static bool ex64_spi_open(void)
{
    ca_spi_config_t cfg = {
        .spi_num    = EX64_SPI,
        .br         = EX64_BR,
        .cpol       = 0,
        .cpha       = 0,
        .master     = true,
        .frame_size = 0,
        .on_rx      = NULL,
        .alternate_gpio = EX64_ALT_GPIO,
        .manage_cs  = false,
    };
    return ca_spi_init_dma(&cfg);
}

static u32 task_loop(void *arg)
{
    (void)arg;
    static const u16 sizes[] = { 1U, 2U, 16U, 64U, 128U, 255U, 256U };

    /* SPI+DMA opened HERE, not in ca_os_configure(). Opened pre-scheduler the
     * FIRST transfer failed every time - ca_spi_dma_xfer() returned 0, meaning
     * ca_dma_copy_target() refused to arm a channel - while every later one
     * passed, 1 byte included. The failure path calls ca_spi_hw_dma_finish(),
     * which restores the peripheral, so the second attempt found it ready:
     * that is why only the cold start was affected. Same shape as the ex21
     * stdout hang, and the same fix - let the task own the bring-up and give
     * it a moment to settle. */
    ca_os_sleep(200);

    if (!ex64_spi_open())
        CA_SAFEGUARD(0, "ex64: init spi dma failed");

    ca_os_sleep(100);

    /* The loop is idle until asked: say so, or the silence looks like a hang. */
    EVAL_STDOUT(
        ca_stdout_put_n("\r\n=== Example 64: SPI DMA loopback ===");
        ca_stdout_put_n("ex64: idle - \"e64 wire\" to check the jumper,"
                        " \"e64 start\" to run one pass");
    )

    while (1) {
        /* Parked: say so, so that loop_park() knows the pins are free. */
        while (s_loop_stop) { s_loop_idle = true; ca_os_sleep(50); }
        s_loop_idle = false;

        /* ONE PASS over every size, then a verdict and back to idle. The
         * example used to cycle for ever, which meant it had no notion of
         * "the run succeeded" - the bench had to guess one from counters
         * that never stopped moving, and every other command competed with
         * the traffic. A pass is a finite, repeatable experiment: same sizes,
         * same order, one line each, one verdict at the end. */
        u32 pass_errs = 0U;

        for (u32 idx = 0U; idx < (sizeof(sizes) / sizeof(sizes[0])); idx++) {
            /* "e64 wire" asked for the pins mid-pass: give them up promptly
             * rather than finishing all seven transfers first. */
            if (s_loop_stop) break;

            u16 len = sizes[idx];

            for (u16 i = 0; i < len; i++)
                s_tx[i] = prng_byte();
            ca_memset(s_rx, 0x00, len);

            u16 n = ca_spi_dma_xfer(EX64_SPI, s_tx, s_rx, len);

            u32 errs = 0U;
            if (n != len) {
                errs = len;                     /* transfer not completed */
            } else {
                for (u16 i = 0; i < len; i++)
                    if (s_rx[i] != s_tx[i]) {
                        if (errs == 0U) {
                            s_err_idx = i;
                            s_err_tx  = s_tx[i];
                            s_err_rx  = s_rx[i];
                        }
                        errs++;
                    }
            }

            s_last_len  = len;
            s_last_errs = errs;
            if (errs == 0U) {
                s_cnt_ok++;
                ca_stdout_printf_n("EX64 DMA loop: %u B  OK", (unsigned)len);
            } else {
                s_cnt_err++;
                pass_errs += errs;
                ca_stdout_printf_n("EX64 DMA loop: %u B  ERR=%u @%u tx=%02X rx=%02X",
                                   (unsigned)len, (unsigned)errs,
                                   (unsigned)s_err_idx, s_err_tx, s_err_rx);
            }

            ca_os_sleep(50);
        }

        /* The verdict for the pass, in the shape the bench already parses for
         * `wire`: fixed prefix, one word, count in brackets. A pass cut short
         * by `wire` is neither OK nor FAIL - say so rather than call a
         * half-run a pass. */
        if (s_loop_stop) {
            ca_stdout_put_n("  RUN RESULT : ABORTED  (0/1)");
        } else {
            ca_stdout_printf_n("  RUN RESULT : %s  (%u/1)",
                               (pass_errs == 0U) ? "OK" : "FAIL",
                               (unsigned)((pass_errs == 0U) ? 1U : 0U));
        }

        /* One pass per "start": park again instead of rolling into the next. */
        s_loop_stop = true;
    }
    return 0;
}

/* ---- CLI "e64" ---- */
#if ENABLE_CLI
static bool ex64_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("e64       : start | info | regs | buf | wire");
        return false;
    }
    if (ca_strncmp(p, "e64", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "start", 5) == 0) {
        s_loop_idle  = false;   /* cleared here: the pass has been asked for */
        s_loop_stop  = false;
        CLI_PUT_N("e64: pass started - 1..256 B, RUN RESULT at the end");
        return true;
    }

    if (ca_strncmp(p, "wire", 4) == 0) {
        /* CONTINUITY test on plain GPIO: no SPI, no DMA, so a FAIL here means
         * the jumper and not the configuration. Every board needs this before
         * anything else - the reference manual says which pins CAN carry SPI1,
         * the board manual says which ones are reachable on a connector, and
         * they are not the same question.
         * MOSI is driven as an output and MISO read back: with the jumper in
         * place the level follows, without it the pin floats. */
        /* Stop the loop first and WAIT for it to confirm: it drives MOSI
         * through the SPI and would fight the levels this test writes. A bare
         * 200 ms sleep was not enough - ca_spi_dma_xfer() blocks, so the loop
         * could still be mid-transfer, and the verdict came out unreadable
         * among its prints. */
        if (!loop_park()) {
            CLI_PUT_N("  WIRE RESULT : FAIL  (0/1)");
            CLI_PUT_N("  (the loop would not stop - a transfer is stuck, so"
                      " the pins could not be measured)");
            return true;
        }

        ca_gpio_configure(EX64_MOSI_PORT, EX64_MOSI_PIN, CA_GPIO_MODE_OUTPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
        ca_gpio_configure(EX64_MISO_PORT, EX64_MISO_PIN, CA_GPIO_MODE_INPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

        ca_gpio_write(EX64_MOSI_PORT, EX64_MOSI_PIN, 0);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 lo = ca_gpio_read(EX64_MISO_PORT, EX64_MISO_PIN);
        ca_gpio_write(EX64_MOSI_PORT, EX64_MOSI_PIN, 1);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 hi = ca_gpio_read(EX64_MISO_PORT, EX64_MISO_PIN);

        CLI_PRINTF_N("  %s : drive0->read%u  drive1->read%u  %s",
                     EX64_WIRE_TXT, lo, hi,
                     (lo == 0U && hi == 1U) ? "OK (wire present)"
                                            : "FAIL (wire missing or swapped)");
        /* One line the bench can read, identical in every example that has a
         * `wire` command. The per-signal lines above stay as they are - they
         * name pins and connector positions and are worth reading by eye - but
         * the VERDICT was written five different ways across the seven
         * examples ("OK (wire present)", "OK (jumper present)", "RESULT: PASS",
         * a bare "FAILED", and ex22 with no verdict at all), so a bench that
         * wants to use `wire` as a precondition would need five parsers.
         * Fixed prefix, one word, count in brackets. */
        CLI_PRINTF_N("  WIRE RESULT : %s  (%u/1)",
                     (lo == 0U && hi == 1U) ? "OK" : "FAIL",
                     (u32)((lo == 0U && hi == 1U) ? 1U : 0U));
        /* The pins are plain GPIO now, not SPI AF, and there is no way to hand
         * them back short of a reboot: ca_spi_init_dma() refuses a second call
         * for an SPI it already holds a DMA slot for (sdma_find() != -1), and
         * nothing releases that slot. Reboot, like every other `wire` in the
         * examples - same wording and same 2.5 s, so the bench sees one shape. */
        CLI_PUT_N("  rebooting in 2.5 s to restore the SPI pins...");
        ca_os_sleep(2500);
        ca_os_reset();
        return true;
    }

    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("ok      : %u   err: %u", s_cnt_ok, s_cnt_err);
        CLI_PRINTF_N("last    : %u B  errs=%u", s_last_len, s_last_errs);
        return true;
    }
    if (ca_strncmp(p, "regs", 4) == 0) {
        /* diagnostic dump of the block: SPI1 + the two GPDMA channels.
         * ch0 = RX (0x40020050), ch1 = TX (0x400200D0). */
        volatile u32 *spi = (volatile u32 *)0x40013000U;
        volatile u32 *rx  = (volatile u32 *)0x40020050U;
        volatile u32 *tx  = (volatile u32 *)0x400200D0U;
        u32 sr = spi[5];
        CLI_PRINTF_N("SPI  CR1=%08x CR2=%08x CFG1=%08x", spi[0], spi[1], spi[2]);
        CLI_PRINTF_N("     CFG2=%08x SR=%08x CTSIZE=%u", spi[3], sr, sr >> 16);
        /* offset/4 from channel base: CSR=4 CCR=5 CTR2=17 CBR1=18 */
        CLI_PRINTF_N("RX   CSR=%08x CCR=%08x CTR2=%08x BNDT=%u",
                     rx[4], rx[5], rx[17], rx[18] & 0xFFFFU);
        CLI_PRINTF_N("TX   CSR=%08x CCR=%08x CTR2=%08x BNDT=%u",
                     tx[4], tx[5], tx[17], tx[18] & 0xFFFFU);
        return true;
    }
    if (ca_strncmp(p, "buf", 3) == 0) {
        /* first 16 bytes of the last transfer: distinguishes disconnected MISO
         * (rx constant 00/FF) from a frame shift (rx == tx phase-shifted). */
        CLI_PRINTF_N("tx: %02X %02X %02X %02X %02X %02X %02X %02X"
                     " %02X %02X %02X %02X %02X %02X %02X %02X",
                     s_tx[0], s_tx[1], s_tx[2],  s_tx[3],  s_tx[4],  s_tx[5],
                     s_tx[6], s_tx[7], s_tx[8],  s_tx[9],  s_tx[10], s_tx[11],
                     s_tx[12], s_tx[13], s_tx[14], s_tx[15]);
        CLI_PRINTF_N("rx: %02X %02X %02X %02X %02X %02X %02X %02X"
                     " %02X %02X %02X %02X %02X %02X %02X %02X",
                     s_rx[0], s_rx[1], s_rx[2],  s_rx[3],  s_rx[4],  s_rx[5],
                     s_rx[6], s_rx[7], s_rx[8],  s_rx[9],  s_rx[10], s_rx[11],
                     s_rx[12], s_rx[13], s_rx[14], s_rx[15]);
        return true;
    }
    CLI_PUT_N("e64       : start | info | regs | buf | wire");
    return false;
}

static ca_cli_node_t s_ex64_cli_node = { ex64_cli_handler, NULL };
#endif

/* ---- entry points ---- */
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

    ca_dma_init();

    /* SPI+DMA are NOT opened here: see the init inside task_loop(). */

    EVAL_CLI( ca_cli_register(&s_ex64_cli_node); )
}

void ca_os_main(void)
{
    ca_os_job("sloop", CA_TH_LEVEL4, task_loop, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* EN_SPI_DMA == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m64a, "=== Example 64 ===");
PRIVATE_STRING(m64b, "EN_SPI_DMA / EN_DMA disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m64a);
    PUT_N(&priv_m64b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_SPI_DMA && EN_DMA */
