// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex93.c
 * @brief          : Example 93 — SPI IRQ SLAVE, paired with example 65 (master)
 ******************************************************************************
 * @details
 * Two STM32 boards connected via 3-wire SPI + GND. Purpose: verify
 * INTEROPERABILITY between a master in DMA (example 65) and a slave in
 * interrupt (this example) on different silicon, under continuous data flow.
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
 * The SLAVE receives the master's frames byte by byte in ISR
 * (ca_spi_recv_handler) and, once a frame of EX93_FRAME bytes is complete, the
 * callback checks the pattern (byte[0] = seq, byte[i] = i + seq) and updates the
 * counters. A seq jump other than 1 (gap) means frames lost.
 *
 * CLI: "s93 info" (frames, errors, last seq/len/gap), "s93 buf" (first 16 B of
 * the last frame against the expected pattern), "s93 wire" (continuity of MISO;
 * SCK and MOSI are the master's half, "s65 wire" there).
 *
 * REQUIREMENTS os_config.h: EN_SPI 1 (exclusive with EN_SPI_DMA)
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
#define EX93_ALT_GPIO  true
#else
#define EX93_ALT_GPIO  false
#endif


#define EX93_SPI     1U
#define EX93_FRAME   64U     /* bytes per frame */
/* Byte this slave drives on MISO for every byte it receives: ex65 checks
 * that every MISO byte of every frame is this one. Same value in ex65. */
#define EX93_MISO_FILL  0x5AU

/* Wiring. With the default SPI1 map every Nucleo-144 brings the three lines
 * to the SAME Arduino position on CN7 - D13 SCK, D12 MISO, D11 MOSI, GND on
 * CN7-8 (UM2861 Table 15, UM2407 Table 18, UM1974) - so the wires run straight
 * across. The H563 is the exception: its Ethernet takes PA5-PA7, SPI1 moves to
 * the alternate PB3/PB4/PB5 and those reach only the morpho CN12 (CN7 is mute
 * there, measured 19/07).
 * The MCU pins differ per family and are what "wire" drives: the H7 driver's
 * default MOSI is PB5 (PA7 is RMII_CRS_DV), every other family uses PA7. */
#if EX93_ALT_GPIO
#define EX93_SCK_PORT   CA_GPIO_PORT_B
#define EX93_SCK_PIN    CA_GPIO_PIN_3
#define EX93_MISO_PORT  CA_GPIO_PORT_B
#define EX93_MISO_PIN   CA_GPIO_PIN_4
#define EX93_MOSI_PORT  CA_GPIO_PORT_B
#define EX93_MOSI_PIN   CA_GPIO_PIN_5
#define EX93_PIN_SCK    "PB3"
#define EX93_PIN_MISO   "PB4"
#define EX93_PIN_MOSI   "PB5"
#define EX93_POS_SCK    "CN12-31"
#define EX93_POS_MISO   "CN12-27"
#define EX93_POS_MOSI   "CN12-29"
#define EX93_POS_GND    "CN12-9"
#else
#define EX93_SCK_PORT   CA_GPIO_PORT_A
#define EX93_SCK_PIN    CA_GPIO_PIN_5
#define EX93_MISO_PORT  CA_GPIO_PORT_A
#define EX93_MISO_PIN   CA_GPIO_PIN_6
#if MCU_ID == 753 || MCU_ID == 747
#define EX93_MOSI_PORT  CA_GPIO_PORT_B
#define EX93_MOSI_PIN   CA_GPIO_PIN_5
#define EX93_PIN_MOSI   "PB5"
#else
#define EX93_MOSI_PORT  CA_GPIO_PORT_A
#define EX93_MOSI_PIN   CA_GPIO_PIN_7
#define EX93_PIN_MOSI   "PA7"
#endif
#define EX93_PIN_SCK    "PA5"
#define EX93_PIN_MISO   "PA6"
#define EX93_POS_SCK    "CN7-10 (D13)"
#define EX93_POS_MISO   "CN7-12 (D12)"
#define EX93_POS_MOSI   "CN7-14 (D11)"
#define EX93_POS_GND    "CN7-8"
#endif

/* CS: Arduino D10 = PD14 on CN7-16 on every Nucleo-144, H563 included
 * (UM2861 Table 15, UM2407 Table 18, UM3115 Table 21). The master drives it
 * as a GPIO around each frame; on the slave an EXTI on its falling edge marks
 * the start of a frame (ca_spi_slave_sync), so a slave that came up in the
 * middle of the stream is aligned again on the next frame. */
#define EX93_CS_PORT    CA_GPIO_PORT_D
#define EX93_CS_PIN     CA_GPIO_PIN_14
#define EX93_PIN_CS     "PD14"
#define EX93_POS_CS     "CN7-16 (D10)"

/* The table "wire" prints first: where each wire goes, on both boards */
#define EX93_WIRE_TABLE()                                                         \
    do {                                                                       \
        CLI_PUT_N("wiring master <-> slave (same position on both boards;");   \
        CLI_PUT_N("H563/F767/F746 use CN12-31/27/29 + CN12-9 for SCK/MISO/MOSI/GND):");      \
        CLI_PUT_N("  SCK   " EX93_POS_SCK  "  this board " EX93_PIN_SCK);            \
        CLI_PUT_N("  MISO  " EX93_POS_MISO "  this board " EX93_PIN_MISO);           \
        CLI_PUT_N("  MOSI  " EX93_POS_MOSI "  this board " EX93_PIN_MOSI);           \
        CLI_PUT_N("  CS    " EX93_POS_CS   "  this board " EX93_PIN_CS);             \
        CLI_PUT_N("  GND   " EX93_POS_GND);                                       \
    } while (0)

/* Set by "s93 wire": the transfer tasks drive these pins through SPI and would
 * overwrite the levels the continuity test writes. Parked, not killed - the
 * reboot at the end of the test restores everything. */
static volatile bool s_wire_stop;

/* Continuity between TWO boards, at static levels. A pin cannot be driven
 * and read on the same board (it is either an output or an input), so each
 * board drives its own lines and reads the other's: the master drives SCK and
 * MOSI and reads MISO, the slave drives MISO and reads SCK and MOSI. Inputs
 * have a pull-down, so a missing wire reads 0 whatever the other side drives:
 * the level 1 finds it, the level 0 finds a line stuck high. */
static inline void ex93_wire_out(ca_gpio_port_t port, ca_gpio_pin_t pin, u8 lvl)
{
    ca_gpio_configure(port, pin, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_write(port, pin, lvl);
}

static inline u8 ex93_wire_in(ca_gpio_port_t port, ca_gpio_pin_t pin)
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
 * byte every 2-3 (s93 buf showed the +1 ramp with jumps), all frames
 * resulted in err and the count slipped. At DIV256 (~625 kHz, one byte every
 * ~13 us) the ISR has abundant margin: err=0, gap=1 stable.
 *
 * For interoperability tests under flow, you can go up in steps
 * (DIV128, DIV64, ...) monitoring "s93 info"/"s93 buf" on the slave: err>0 or
 * gap!=1 signal the start of the overrun, i.e., the rate limit of the ISR
 * byte-per-byte on that pair of boards. To go faster, you would need to
 * move the verification out of the ISR (handler = accumulation only, task = verification)
 * or switch the slave to DMA. */
#define EX93_BR      CA_SPI_BR_DIV256
#define EX93_PERIOD  500U    /* ms between one frame and the next */

/* ============================================================================
 * SLAVE — SPI IRQ
 * ============================================================================ */
#if EN_SPI
#include "spi.h"

static volatile u32 s_frames;     /* frames received           */
static volatile u32 s_errs;       /* frames with wrong pattern */
static volatile u8  s_last_seq;   /* seq of the last frame    */
static volatile u8  s_last_gap;   /* seq jump (lost frames) */
static volatile u16 s_last_len;   /* length of last frame   */
static u8           s_last_buf[16];   /* first 16 B of last frame (diagnostics) */
static volatile u16 s_first_err;      /* index of first wrong byte, 0xFFFF=none */

/* ISR Callback — complete frame (EX93_FRAME bytes) received.
 * Runs in interrupt context: ONLY counts, no I/O. Printing is done by
 * task_report: ca_stdout_printf_n queues with ca_queue_static_send at timeout 0,
 * which is BLOCKING (does not drop) — calling it from ISR means potentially blocking
 * the interrupt when the queue is full, and it is precisely under fast flow that
 * the queue fills up. */
static void slave_cb(u8 spi_num, const u8 *data, u16 len)
{
    (void)spi_num;
    u8 seq = data[0];
    u32 errs = 0U;
    u16 first_err = 0xFFFFU;
    for (u16 i = 1; i < len; i++)
        if (data[i] != (u8)((i + seq) & 0xFFU)) {
            if (first_err == 0xFFFFU) first_err = i;
            errs++;
        }

    /* snapshot first 16 B: distinguishes frame shift (pattern correct but
     * translated) from bytes lost due to OVR (holes/jumps in the sequence) */
    for (u16 i = 0; i < 16U && i < len; i++)
        s_last_buf[i] = data[i];
    s_first_err = first_err;

    /* seq is an 8-bit counter incremented by 1 by the master: a gap
     * other than 1 means lost frames, the typical symptom when increasing the baud */
    if (s_frames) s_last_gap = (u8)(seq - s_last_seq);
    s_last_seq = seq;
    s_last_len = len;
    s_frames++;
    if (errs) s_errs++;
}

/* CS falling edge (EXTI, interrupt context): a frame starts. Drop whatever
 * partial frame and overrun the slave was left with. */
static volatile u32 s_cs_edges;     /* real CS falling edges (frame starts) */
static volatile u32 s_cs_glitch;    /* edges with CS already back high      */
static volatile u32 s_cs_late;      /* edges with a byte already waiting: the
                                     * EXTI ran after the frame had started */
static void cs_falling(void)
{
    /* A real CS stays low for the whole frame (~1 ms). The SPI traffic
     * couples nanosecond dips into the line - ground bounce on the one GND
     * jumper between the boards: ~2 false edges per byte on U575 -> H753,
     * 13/10, each one resetting the byte count so no frame ever completed.
     * By the time this ISR runs a dip is over: read the pin again. */
    if (ca_gpio_read(EX93_CS_PORT, EX93_CS_PIN) != 0U) {
        s_cs_glitch++;
        return;
    }
    s_cs_edges++;
    ca_spi_slave_sync(EX93_SPI);
    if (ca_spi_hw_slave_sync_pending(EX93_SPI)) s_cs_late++;
}

/* Print outside the ISR. If multiple frames arrive between two passes, the lines
 * merge: this is intentional, at high speed the console must not act as a
 * bottleneck — the counters remain exact. */
static u32 task_report(void *arg)
{
    (void)arg;
    u32 shown = 0U;

    while (1) {
        while (s_wire_stop) ca_os_sleep(100);

        u32 n = s_frames;
        if (n != shown) {
            shown = n;
            ca_stdout_printf_n("EX93 S: seq=%u len=%u gap=%u  tot=%u err=%u",
                               (unsigned)s_last_seq, (unsigned)s_last_len,
                               (unsigned)s_last_gap, (unsigned)n,
                               (unsigned)s_errs);
        }
        ca_os_sleep(100);
    }
    return 0;
}

#if ENABLE_CLI
static bool ex93_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("s93: info | buf | wire 0|1|end"); return false; }
    if (ca_strncmp(p, "s93", 3) != 0) return false;
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
            EX93_WIRE_TABLE();
            CLI_PUT_N("s93 wire 0|1 : drive MISO to 0|1, read SCK, MOSI and CS");
            CLI_PUT_N("s65 wire 0|1 on the master: drive SCK, MOSI and CS, read MISO");
            CLI_PUT_N("s93 wire end : reboot (SPI back on the pins)");
            return true;
        }
        const u8 lvl = (u8)(*p - '0');
        s_wire_stop = true;
        ca_os_sleep(100);
        ex93_wire_out(EX93_MISO_PORT, EX93_MISO_PIN, lvl);
        const u8 sck  = ex93_wire_in(EX93_SCK_PORT,  EX93_SCK_PIN);
        const u8 mosi = ex93_wire_in(EX93_MOSI_PORT, EX93_MOSI_PIN);
        const u8 cs   = ex93_wire_in(EX93_CS_PORT,   EX93_CS_PIN);
        CLI_PRINTF_N("WIRE S: drive MISO=%u  read SCK=%u MOSI=%u CS=%u   (" EX93_PIN_MISO
                     " out, " EX93_PIN_SCK "/" EX93_PIN_MOSI "/" EX93_PIN_CS " in)",
                     lvl, sck, mosi, cs);
        return true;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("SLAVE  frames: %u  err: %u", s_frames, s_errs);
        CLI_PRINTF_N("       last_seq: %u  last_len: %u  last_gap: %u",
                     s_last_seq, s_last_len, s_last_gap);
        CLI_PRINTF_N("       cs edges: %u  glitches: %u  late: %u",
                     s_cs_edges, s_cs_glitch, s_cs_late);
        return true;
    }
    if (ca_strncmp(p, "buf", 3) == 0) {
        /* rx = first 16 B received; exp = expected pattern given seq=rx[0].
         * If rx == exp but err on info → shift BEYOND byte 16 or OVR at end of
         * frame. If rx[0] "plausible" but rx[1..] shifted by k = master
         * clocked k bytes before the slave hooked on. */
        u8 seq = s_last_buf[0];
        CLI_PRINTF_N("rx : %02X %02X %02X %02X %02X %02X %02X %02X"
                     " %02X %02X %02X %02X %02X %02X %02X %02X",
                     s_last_buf[0], s_last_buf[1], s_last_buf[2], s_last_buf[3],
                     s_last_buf[4], s_last_buf[5], s_last_buf[6], s_last_buf[7],
                     s_last_buf[8], s_last_buf[9], s_last_buf[10], s_last_buf[11],
                     s_last_buf[12], s_last_buf[13], s_last_buf[14], s_last_buf[15]);
        CLI_PRINTF_N("exp: %02X %02X %02X %02X %02X %02X %02X %02X"
                     " %02X %02X %02X %02X %02X %02X %02X %02X",
                     seq,
                     (u8)((1+seq)&0xFF),  (u8)((2+seq)&0xFF),  (u8)((3+seq)&0xFF),
                     (u8)((4+seq)&0xFF),  (u8)((5+seq)&0xFF),  (u8)((6+seq)&0xFF),
                     (u8)((7+seq)&0xFF),  (u8)((8+seq)&0xFF),  (u8)((9+seq)&0xFF),
                     (u8)((10+seq)&0xFF), (u8)((11+seq)&0xFF), (u8)((12+seq)&0xFF),
                     (u8)((13+seq)&0xFF), (u8)((14+seq)&0xFF), (u8)((15+seq)&0xFF));
        CLI_PRINTF_N("first_err idx: %u", s_first_err);
        return true;
    }
    CLI_PUT_N("s93: info | buf | wire 0|1|end");
    return false;
}
static ca_cli_node_t s_cli = { ex93_cli, NULL };
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

    ca_spi_config_t cfg = {
        .spi_num = EX93_SPI, .br = 0,      /* slave: baud ignored */
        .cpol = 0, .cpha = 0, .master = false,
        .frame_size = EX93_FRAME, .on_rx = slave_cb, .manage_cs = false,
        .alternate_gpio = EX93_ALT_GPIO,
        .slave_tx_fill = EX93_MISO_FILL,   /* the master (ex65) checks it */
    };
    if (!ca_spi_init(&cfg))
        CA_SAFEGUARD(0, "ex93: init spi slave failed");
    /* CS from the master: pull-up so it idles high without a master; its
     * falling edge (frame start) realigns the slave. */
    ca_gpio_configure(EX93_CS_PORT, EX93_CS_PIN, CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_PULLUP, 0);
    if (!ca_exti_init((u8)EX93_CS_PORT, 14U, CA_EXTI_EDGE_FALLING, cs_falling))
        CA_SAFEGUARD(0, "ex93: CS EXTI init failed");
    EVAL_CLI( ca_cli_register(&s_cli); )
}

void ca_os_main(void)
{
    ca_os_job("s93", CA_TH_LEVEL4, task_report, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* EN_SPI == 0 */
/* Pre-scheduler diagnostic: print straight to the console backend (PUT_N),
 * ca_stdout_* would fail with TLB=NULL before ca_os_run. */
PRIVATE_STRING(m93s1, "=== Example 93 (slave) ===");
PRIVATE_STRING(m93s2, "EN_SPI disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m93s1);
    PUT_N(&priv_m93s2);
}
void ca_os_main(void) { ca_os_run(true); }
#endif /* EN_SPI */
