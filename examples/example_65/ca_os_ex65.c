// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex65.c
 * @brief          : Example 65 — SPI DMA MASTER, paired with example 93 (slave)
 ******************************************************************************
 * @details
 * Two STM32 boards connected via 3-wire SPI + GND. Purpose: verify
 * INTEROPERABILITY between a master in DMA (this example) and a slave in
 * interrupt (example 93) on different silicon, under continuous data flow.
 *
 * Until 13/10/2026 master and slave were one example with the role picked from
 * MCU_ID; the two roles need MUTUALLY EXCLUSIVE os_config flags (EN_SPI_DMA vs
 * EN_SPI), so one config could only ever build one of them and the other board
 * came up silent. Split: ex65 = master on any board, ex93 = slave on any board.
 * The role is now which example is flashed.
 *
 * ---- REFERENCE PAIR: NUCLEO-H563ZI (master) + NUCLEO-H753ZI (slave) --
 *
 * WARNING: the two boards use DIFFERENT pin mappings, so the wires do NOT
 * connect homonymous pins. The H563 has Ethernet on the default SPI1 pins and
 * uses the alternative mapping (alternate_gpio=true, PB3/PB4/PB5); the H753 uses
 * the h7xx driver default (PA5/PA6/PB5). All connections on the MORPHO
 * CN12 of both boards — DO NOT use the Arduino CN7 connectors (see
 * doc/manual/S03_64_00_example_64.md: more pins there are disconnected by factory
 * open solder bridges).
 *
 *   signal    H563 master (alt)        H753 slave (default)
 *   -------   ---------------------    ---------------------
 *   SCK       PB3  -> CN12 pin 31  ──  CN12 pin 11 <- PA5
 *   MISO      PB4  -> CN12 pin 27  ──  CN12 pin 13 <- PA6
 *   MOSI      PB5  -> CN12 pin 29  ──  CN12 pin 29 <- PB5
 *   GND              CN12 pin  9   ──  CN12 pin  9
 *
 * ---- ALTERNATIVE PAIR: NUCLEO-U575ZI-Q (master) + STM32F4VE (slave) --------
 *
 * Both on DEFAULT SPI1 mapping (alternate_gpio=false): neither of them
 * has Ethernet occupying PA5/PA6/PA7, so here the pins ARE homonymous.
 *
 *   signal    U575 master (default)         F4VE slave (default)
 *   -------   --------------------------    ---------------------
 *   SCK       PA5  -> ZIO CN7 pin 10 (D13) ── PA5 (side header)
 *   MISO      PA6  -> ZIO CN7 pin 12 (D12) ── PA6
 *   MOSI      PA7  -> ZIO CN7 pin 14 (D11) ── PA7
 *   GND              CN7 GND                ── GND
 *   (U575: pinout UM2861 Table 15, Zio CN7 verified on ex64 20/07)
 *
 *   (NSS software: SSM=1 on both, slave always active — no CS wire)
 *
 *
 * The MASTER sends a frame of EX65_FRAME bytes every EX65_PERIOD ms in DMA mode
 * (ca_spi_dma_xfer, blocking until TC): byte[0] = progressive counter,
 * byte[1..] = pattern (i + seq). The slave (ex93) checks the pattern.
 *
 * CLI: "s65 info" (frames sent), "s65 wire" (continuity of SCK and MOSI; MISO
 * is the slave's half, "s93 wire" there).
 *
 * REQUIREMENTS os_config.h: EN_SPI_DMA 1 (requires EN_DMA; exclusive with EN_SPI)
 ******************************************************************************
 */

#include "caos.h"

/* The alternate map must be known BEFORE the pin block below picks the "wire"
 * pins from it: it used to be defined after, so the #if read it as 0 and the
 * H563 wire test probed PA5/PA6/PA7 while its SPI ran on PB3/PB4/PB5. */
/* NUCLEO-H563ZI: PA7 = RMII_CRS_DV (SB38 ON) → SPI1 on the mapping
 * alternative PB3=SCK, PB4=MISO, PB5=MOSI. */
/* Ethernet on the Nucleo-144 takes PA7 (RMII_CRS_DV / RMII_DV on D11): H563
 * (also PA5-PA7 per UM3115), F767 and F746 (UM1974 note 2: PA7 is D11 AND,
 * with JP6 ON, the PHY's RMII_DV). Those boards move SPI1 to PB3/PB4/PB5,
 * which reach the morpho CN12-31/27/29 on both board families. */
#if MCU_ID == 563 || MCU_ID == 767 || MCU_ID == 746
#define EX65_ALT_GPIO  true
#else
#define EX65_ALT_GPIO  false
#endif


#define EX65_SPI     1U
#define EX65_FRAME   64U     /* bytes per frame */
/* Byte the slave (ex93) drives on MISO for every byte it receives: every
 * MISO byte of every frame must be this one. Same value in ex93. */
#define EX65_MISO_FILL  0x5AU

/* Wiring. With the default SPI1 map every Nucleo-144 brings the three lines
 * to the SAME Arduino position on CN7 - D13 SCK, D12 MISO, D11 MOSI, GND on
 * CN7-8 (UM2861 Table 15, UM2407 Table 18, UM1974) - so the wires run straight
 * across. The H563 is the exception: its Ethernet takes PA5-PA7, SPI1 moves to
 * the alternate PB3/PB4/PB5 and those reach only the morpho CN12 (CN7 is mute
 * there, measured 19/07).
 * The MCU pins differ per family and are what "wire" drives: the H7 driver's
 * default MOSI is PB5 (PA7 is RMII_CRS_DV), every other family uses PA7. */
#if EX65_ALT_GPIO
#define EX65_SCK_PORT   CA_GPIO_PORT_B
#define EX65_SCK_PIN    CA_GPIO_PIN_3
#define EX65_MISO_PORT  CA_GPIO_PORT_B
#define EX65_MISO_PIN   CA_GPIO_PIN_4
#define EX65_MOSI_PORT  CA_GPIO_PORT_B
#define EX65_MOSI_PIN   CA_GPIO_PIN_5
#define EX65_PIN_SCK    "PB3"
#define EX65_PIN_MISO   "PB4"
#define EX65_PIN_MOSI   "PB5"
#define EX65_POS_SCK    "CN12-31"
#define EX65_POS_MISO   "CN12-27"
#define EX65_POS_MOSI   "CN12-29"
#define EX65_POS_GND    "CN12-9"
#else
#define EX65_SCK_PORT   CA_GPIO_PORT_A
#define EX65_SCK_PIN    CA_GPIO_PIN_5
#define EX65_MISO_PORT  CA_GPIO_PORT_A
#define EX65_MISO_PIN   CA_GPIO_PIN_6
#if MCU_ID == 753 || MCU_ID == 747
#define EX65_MOSI_PORT  CA_GPIO_PORT_B
#define EX65_MOSI_PIN   CA_GPIO_PIN_5
#define EX65_PIN_MOSI   "PB5"
#else
#define EX65_MOSI_PORT  CA_GPIO_PORT_A
#define EX65_MOSI_PIN   CA_GPIO_PIN_7
#define EX65_PIN_MOSI   "PA7"
#endif
#define EX65_PIN_SCK    "PA5"
#define EX65_PIN_MISO   "PA6"
#define EX65_POS_SCK    "CN7-10 (D13)"
#define EX65_POS_MISO   "CN7-12 (D12)"
#define EX65_POS_MOSI   "CN7-14 (D11)"
#define EX65_POS_GND    "CN7-8"
#endif

/* CS: Arduino D10 = PD14 on CN7-16 on every Nucleo-144, H563 included
 * (UM2861 Table 15, UM2407 Table 18, UM3115 Table 21). The master drives it
 * as a GPIO around each frame; on the slave an EXTI on its falling edge marks
 * the start of a frame (ca_spi_slave_sync), so a slave that came up in the
 * middle of the stream is aligned again on the next frame. */
#define EX65_CS_PORT    CA_GPIO_PORT_D
#define EX65_CS_PIN     CA_GPIO_PIN_14
#define EX65_PIN_CS     "PD14"
#define EX65_POS_CS     "CN7-16 (D10)"

/* The table "wire" prints first: where each wire goes, on both boards */
#define EX65_WIRE_TABLE()                                                         \
    do {                                                                       \
        CLI_PUT_N("wiring master <-> slave (same position on both boards;");   \
        CLI_PUT_N("H563/F767/F746 use CN12-31/27/29 + CN12-9 for SCK/MISO/MOSI/GND):");      \
        CLI_PUT_N("  SCK   " EX65_POS_SCK  "  this board " EX65_PIN_SCK);            \
        CLI_PUT_N("  MISO  " EX65_POS_MISO "  this board " EX65_PIN_MISO);           \
        CLI_PUT_N("  MOSI  " EX65_POS_MOSI "  this board " EX65_PIN_MOSI);           \
        CLI_PUT_N("  CS    " EX65_POS_CS   "  this board " EX65_PIN_CS);             \
        CLI_PUT_N("  GND   " EX65_POS_GND);                                       \
    } while (0)

/* Set by "s65 wire": the transfer tasks drive these pins through SPI and would
 * overwrite the levels the continuity test writes. Parked, not killed - the
 * reboot at the end of the test restores everything. */
static volatile bool s_wire_stop;

/* Continuity between TWO boards, at static levels. A pin cannot be driven
 * and read on the same board (it is either an output or an input), so each
 * board drives its own lines and reads the other's: the master drives SCK and
 * MOSI and reads MISO, the slave drives MISO and reads SCK and MOSI. Inputs
 * have a pull-down, so a missing wire reads 0 whatever the other side drives:
 * the level 1 finds it, the level 0 finds a line stuck high. */
static inline void ex65_wire_out(ca_gpio_port_t port, ca_gpio_pin_t pin, u8 lvl)
{
    ca_gpio_configure(port, pin, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_write(port, pin, lvl);
}

static inline u8 ex65_wire_in(ca_gpio_port_t port, ca_gpio_pin_t pin)
{
    ca_gpio_configure(port, pin, CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    return ca_gpio_read(port, pin);
}

/* Speed and cadence.
 *
 * DIV256 is a PRUDENT default, chosen because the slave is the bottleneck:
 * it receives ONE byte per interrupt (ca_spi_hw_recv_handler reads SPI->DR
 * at every RXNE) and every 64 bytes it executes the verification callback inside the ISR. If
 * the master clocks faster than the slave can empty SPI->DR, the
 * unread byte goes into OVERRUN: the next data overwrites it and it is lost.
 *
 * With the test pair (master U575 PCLK ~160 MHz / slave F407 168 MHz) at
 * DIV64 (~2.5 MHz, one byte every ~3 us) the slave could NOT keep up: it lost ~1
 * byte every 2-3 (s65 buf showed the +1 ramp with jumps), all frames
 * resulted in err and the count slipped. At DIV256 (~625 kHz, one byte every
 * ~13 us) the ISR has abundant margin: err=0, gap=1 stable.
 *
 * For interoperability tests under flow, you can go up in steps
 * (DIV128, DIV64, ...) monitoring "s65 info"/"s65 buf" on the slave: err>0 or
 * gap!=1 signal the start of the overrun, i.e., the rate limit of the ISR
 * byte-per-byte on that pair of boards. To go faster, you would need to
 * move the verification out of the ISR (handler = accumulation only, task = verification)
 * or switch the slave to DMA. */
#define EX65_BR      CA_SPI_BR_DIV256
#define EX65_PERIOD  500U    /* ms between one frame and the next */

/* ============================================================================
 * MASTER — SPI DMA
 * ============================================================================ */
#if EN_SPI_DMA && EN_DMA
#include "spi.h"

static u8 s_tx[EX65_FRAME] __attribute__((aligned(4)));
static u8 s_rx[EX65_FRAME] __attribute__((aligned(4)));  /* MISO, checked */

static volatile u32 s_seq;        /* frames sent       */
static volatile u32 s_miso_ok;    /* frames whose MISO is all EX65_MISO_FILL */
static volatile u32 s_miso_bad;   /* frames with at least one other byte    */
static volatile u8  s_miso_last;  /* first wrong MISO byte of the last bad frame */
static volatile u32 s_frame_us;   /* duration of the last frame on the wire  */
static volatile u32 s_slow_us;    /* 0 = DMA burst; >0 = byte by byte, polling,
                                   * with this many us between bytes ("s65 slow") */

static u32 task_master(void *arg)
{
    (void)arg;
    ca_os_sleep(100);

    while (1) {
        while (s_wire_stop) ca_os_sleep(100);

        u8 seq = (u8)s_seq;
        s_tx[0] = seq;
        for (u16 i = 1; i < EX65_FRAME; i++)
            s_tx[i] = (u8)((i + seq) & 0xFFU);

        /* CS low marks the frame start for the slave; a short wait lets its
         * EXTI realign it before the first clock */
        ca_gpio_write(EX65_CS_PORT, EX65_CS_PIN, 0U);
        for (volatile u32 d = 0; d < 2000U; d++) { }
        const u32 cyc_us = SystemCoreClock / 1000000U;
        const u32 t0 = DWT->CYCCNT;
        if (s_slow_us == 0U) {
            ca_spi_dma_xfer(EX65_SPI, s_tx, s_rx, EX65_FRAME);
        } else {
            /* Same SCK inside a byte, but the slave gets s_slow_us between two
             * bytes: separates "the slave cannot keep up" from "the signal is
             * bad" - only the first one gets better here. */
            for (u16 i = 0; i < EX65_FRAME; i++) {
                s_rx[i] = ca_spi_xfer(EX65_SPI, s_tx[i]);
                const u32 tb = DWT->CYCCNT;
                while ((DWT->CYCCNT - tb) < s_slow_us * cyc_us) { }
            }
        }
        s_frame_us = (DWT->CYCCNT - t0) / cyc_us;
        ca_gpio_write(EX65_CS_PORT, EX65_CS_PIN, 1U);
        s_seq++;

        /* The return half: a slave that answers drives EX65_MISO_FILL on
         * every byte. A missing MISO wire reads a constant 0x00 or 0xFF. */
        u32 bad = 0U;
        for (u16 i = 0; i < EX65_FRAME; i++) {
            if (s_rx[i] != EX65_MISO_FILL) {
                if (bad == 0U) s_miso_last = s_rx[i];
                bad++;
            }
        }
        if (bad) s_miso_bad++; else s_miso_ok++;

        ca_stdout_printf_n("EX65 M: frame %u sent (%u B) miso=%s",
                           (unsigned)s_seq, (unsigned)EX65_FRAME, bad ? "BAD" : "ok");
        ca_os_sleep(EX65_PERIOD);
    }
    return 0;
}

#if ENABLE_CLI
static bool ex65_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("s65: info | slow <us> | wire 0|1|end"); return false; }
    if (ca_strncmp(p, "s65", 3) != 0) return false;
    p = ca_skip_sp(p + 3);
    if (ca_strncmp(p, "wire", 4) == 0) {
        p = ca_skip_sp(p + 4);
        if (ca_strncmp(p, "end", 3) == 0) {
            CLI_PUT_N("wire: end - rebooting to give the pins back to the SPI");
            ca_os_sleep(300);
            ca_os_reset();
            return true;
        }
        if (*p != '0' && *p != '1') {
            EX65_WIRE_TABLE();
            CLI_PUT_N("s65 wire 0|1 : drive SCK, MOSI and CS to 0|1, read MISO");
            CLI_PUT_N("s93 wire 0|1 on the slave: drive MISO, read SCK, MOSI and CS");
            CLI_PUT_N("s65 wire end : reboot (SPI back on the pins)");
            return true;
        }
        const u8 lvl = (u8)(*p - '0');
        s_wire_stop = true;              /* the master task parks after its frame */
        ca_os_sleep(EX65_PERIOD + 100U);
        ex65_wire_out(EX65_SCK_PORT,  EX65_SCK_PIN,  lvl);
        ex65_wire_out(EX65_MOSI_PORT, EX65_MOSI_PIN, lvl);
        ex65_wire_out(EX65_CS_PORT,   EX65_CS_PIN,   lvl);
        const u8 miso = ex65_wire_in(EX65_MISO_PORT, EX65_MISO_PIN);
        CLI_PRINTF_N("WIRE M: drive SCK=MOSI=CS=%u  read MISO=%u   (" EX65_PIN_SCK "/"
                     EX65_PIN_MOSI "/" EX65_PIN_CS " out, " EX65_PIN_MISO " in)", lvl, miso);
        return true;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("MASTER  frames sent: %u", s_seq);
        CLI_PRINTF_N("        miso ok: %u  miso bad: %u  last bad byte: %02X (want %02X)",
                     s_miso_ok, s_miso_bad, s_miso_last, EX65_MISO_FILL);
        /* Only the byte-by-byte time is printed: during a DMA burst the task
         * waits on a semaphore, the core sleeps in WFI, and on the H753 CYCCNT
         * stops there - it read 25 us for a 64 B frame that takes ~0.7 ms. */
        if (s_slow_us != 0U)
            CLI_PRINTF_N("        frame %u B in %u us (byte by byte, %u us gap)",
                         EX65_FRAME, s_frame_us, s_slow_us);
        return true;
    }
    if (ca_strncmp(p, "slow", 4) == 0) {
        const i32 us = ca_strtol(ca_skip_sp(p + 4), 10);
        s_slow_us = (us > 0 && us <= 100000) ? (u32)us : 0U;
        if (s_slow_us) CLI_PRINTF_N("s65 slow: byte by byte, %u us between bytes", s_slow_us);
        else           CLI_PUT_N("s65 slow: off, DMA burst");
        return true;
    }
    CLI_PUT_N("s65: info | slow <us> | wire 0|1|end");
    return false;
}
static ca_cli_node_t s_cli = { ex65_cli, NULL };
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

    ca_dma_init();
    ca_spi_config_t cfg = {
        .spi_num = EX65_SPI, .br = EX65_BR,
        .cpol = 0, .cpha = 0, .master = true,
        .frame_size = 0, .on_rx = NULL, .manage_cs = false,
        .alternate_gpio = EX65_ALT_GPIO,
    };
    if (!ca_spi_init_dma(&cfg))
        CA_SAFEGUARD(0, "ex65: init spi dma master failed");
    /* CS idle high, low around every frame (task_master) */
    ca_gpio_configure(EX65_CS_PORT, EX65_CS_PIN, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_write(EX65_CS_PORT, EX65_CS_PIN, 1U);
    EVAL_CLI( ca_cli_register(&s_cli); )
}

void ca_os_main(void)
{
    ca_os_job("m65", CA_TH_LEVEL4, task_master, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* EN_SPI_DMA == 0 → master build without the flags */
/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N — the same
 * path the kernel uses for its own boot-time messages. */
PRIVATE_STRING(m65m1, "=== Example 65 (master) ===");
PRIVATE_STRING(m65m2, "EN_SPI_DMA / EN_DMA disabled");
void ca_os_configure(void)
{
    PUT_N(&priv_m65m1);
    PUT_N(&priv_m65m2);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_SPI_DMA && EN_DMA */
