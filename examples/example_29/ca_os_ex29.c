// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 29 — SPI master-slave on the same MCU
 *
 * Hardware: NUCLEO-H753ZI, NUCLEO-U575ZI-Q and NUCLEO-H563ZI
 *
 * Wiring (3 wires, no NSS pin) — master on the Arduino CN7, slave on the Zio
 * CN8, three even pins in a row:
 *   SPI1 SCK  PA5       CN7-10 ---- CN8-6   PC10  SCK  SPI3
 *   SPI1 MOSI see below CN7-14 ---- CN8-10  PC12  MOSI SPI3
 *   SPI1 MISO see below CN7-12 ---- CN8-8   PC11  MISO SPI3
 *   (Software NSS: SSM=1 on both, slave always active)
 *
 * PC10/PC11/PC12 are the board's SDMMC1 signals and are NOT on CN7: they come
 * out on the Zio CN8 (pins 6/8/10), and also on the morpho CN11 (pins 1/2/3)
 * if that is more convenient. The header of this example used to say
 * CN7-1/3/5, which is wrong on both boards and made the "wire" test report
 * three dead jumpers on wires that were exactly where the text said to put
 * them. UM2861 Table 16 (U575) and UM2407 (H753) agree on this.
 *
 * The MCU pins differ but the connector positions do not: on the boards with
 * an Ethernet PHY (H753ZI, H563ZI) PA7 is RMII_CRS_DV, so D11 is moved to PB5
 * by SB121/SB122; the boards without one (U575ZI-Q, F446ZE) keep the factory
 * default and D11 is PA7. Both land on CN7 pin 14, so the jumper is
 * physically identical. The H563ZI moves MISO too — PG9 instead of PA6, still
 * on CN7-12 — because PA6 comes out only on the unsoldered morpho there.
 * The DRIVER already picks the right pin per family (hw/h7xx, hw/u5xx and
 * hw/h5xx differ in their default branch): the example does not set
 * alternate_gpio, and must not.
 *
 * Type "wire" on the console to test the three jumpers as plain GPIO before
 * blaming the SPI configuration.
 *
 * PHASE 1 — POLLING (EN_SPI_EX_POLL defined):
 *   task_master  : sends POLL_FRAME_SIZE bytes every 500 ms (pattern i & 0xFF)
 *   task_slave   : receives the same bytes in polling, verifies and logs
 *   Synchronization: volatile flag g_slave_ready (slave set BEFORE entering
 *   polling, master waits with ca_os_yield before transmitting)
 *
 * PHASE 2 — IRQ (EN_SPI_EX_POLL not defined):
 *   task_master  : sends SPI_DEF_PKT_SIZE bytes every 1000 ms
 *   slave_irq_cb : called by SPI3 ISR when frame is fully received
 *
 * REQUIREMENTS os_config.h:
 *   EN_SPI 1
 *
 * DMA NOTE: with SPI_DEF_PKT_SIZE > ~32 bytes and high clock, consider
 *   DMA to avoid IRQ/polling overhead (point 3 of the SPI driver roadmap).
 */

#include "caos.h"

/* This example drives CN7-14 (D11) as a plain GPIO to check jumper
 * continuity, and that pin is PA7 or PB5 depending on the board's solder
 * bridges (UM1974 Table 12, SB121/SB122). A board whose bridges nobody has
 * checked is out of scope: EX29_OK stays 0 and the "not supported" branch at
 * the bottom says so, rather than an #error breaking the build of every other
 * example. Same shape as ex60. */
#if EN_SPI
#define EX29_OK   1
#else
#define EX29_OK   0
#endif

#include "spi.h"

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */

/* Comment out to switch to IRQ mode */
/* #define EN_SPI_EX_POLL */

/* Define for master loopback test: MOSI → MISO (the pins per board above).
 * Overwrites EN_SPI_EX_POLL — no slave started. */
/*#define EN_SPI_LOOPBACK */

#define POLL_FRAME_SIZE   8U    /* small: enters the H7 FIFO (16 bytes) */
#define IRQ_FRAME_SIZE    SPI_DEF_PKT_SIZE

#define MASTER_SPI        1U    /* SPI1 */
#define SLAVE_SPI         3U    /* SPI3 - PC10/PC11/PC12 on both boards */

/* ============================================================================
 * PER-MCU MAPPING — only SPI1_MOSI changes, and only the pin, not the
 * connector position. Labels only: the peripheral numbers and the driver's
 * pin choice are the same everywhere.
 * ========================================================================== */

/* CN7-14 (D11) is PA7 or PB5 depending on a SOLDER BRIDGE, and the factory
 * setting is PA7.
 *
 * UM1974 Table 12: "SB121, SB122 (D11)" is ON,OFF by default (bold in the
 * table), which connects pin 14 of CN7 to PA7; only OFF,ON moves it to PB5.
 * So on an untouched NUCLEO-144 a wire in CN7-14 lands on PA7 whatever the
 * firmware drives.
 *
 * PB5 is right only where PA7 has been taken away from that connector, and
 * what takes it away is the Ethernet PHY: on the H753ZI (and H563ZI) PA7 is
 * RMII_CRS_DV with SB13/SB160 ON, so those boards move D11 to PB5. The
 * NUCLEO-F446ZE has no PHY at all, so PA7 stays exactly where the connector
 * table says it is.
 *
 * The old test was "is this a U575?", which let F446 fall into the H753
 * branch and drive PB5 - a pin that is not on CN7 at all there. The jumper
 * was in the right hole and MOSI still read 0 every time, while SCK and MISO
 * passed, which reads like a broken wire and is not (F446, 2026-09-07).
 *
 * The question is therefore about the PHY, not about the part number.
 *
 * EVERY board is named, and an unlisted one stops the build rather than
 * inheriting somebody else's pin. There is no #else that quietly picks a
 * default here: that is exactly what put F446 on PB5 - it was never chosen
 * for F446, it was simply what the branch for the H753 happened to hold, and
 * the mistake was invisible because the code compiled and ran and only the
 * wire test disagreed. A board added later must state which pin its CN7-14
 * carries, and the compiler is the one that asks. */
#if   MCU_ID == 753 || MCU_ID == 563 || MCU_ID == 747
  /* Ethernet PHY fitted: PA7 is RMII_CRS_DV, so SB121/SB122 move D11 to PB5. */
  #define MOSI_PORT     CA_GPIO_PORT_B
  #define MOSI_PIN      CA_GPIO_PIN_5
  #define MOSI_TXT      "PB5"
#elif MCU_ID == 575 || MCU_ID == 446 || MCU_ID == 767
  /* CN7-14 keeps the factory default of UM1974 Table 12 (SB121 ON): PA7.
   *
   * The F767ZI has the Ethernet PHY and it is tempting to group it with
   * H753/H563, but note 2 of Table 19 (the F746ZG/F756ZG/F767ZI row) says
   * the opposite: "PA7 is used as D11 and connected to CN7 pin 14 BY
   * DEFAULT, if JP6 is ON it is ALSO connected to both Ethernet PHY as RMII_DV
   * and CN9 pin 15. In this case only one function of the Ethernet or D11 must
   * be used." Here the PHY SHARES the pin through a jumper, it does not take
   * it: with the jumper open CN7-14 stays PA7 as on the F446. On H753/H563
   * instead SB13/SB160 give PA7 to the PHY for good and D11 moves to PB5. */
  #define MOSI_PORT     CA_GPIO_PORT_A
  #define MOSI_PIN      CA_GPIO_PIN_7
  #define MOSI_TXT      "PA7"
#else
  /* CN7-14 not characterised for this board: the example does not apply.
   * Read UM1974 Table 12 (SB121/SB122) for which of PA7/PB5 it carries and
   * add it to one of the two lists above - never to the #else, which is how
   * F446 came to drive PB5: it was never chosen for F446, it was simply what
   * the H753 branch happened to hold. The code built, the SPI worked, and
   * only the wire test disagreed - MOSI read 0 with the jumper in the right
   * hole (2026-09-07). */
  #undef  EX29_OK
  #define EX29_OK  0
  #define MOSI_PORT     CA_GPIO_PORT_A
  #define MOSI_PIN      CA_GPIO_PIN_7
  #define MOSI_TXT      "PA7"
#endif

#define SCK_PORT      CA_GPIO_PORT_A       /* PA5  master SCK  */
#define SCK_PIN       CA_GPIO_PIN_5

/* MISO also changes with the board, for the same reason as MOSI: on the
 * NUCLEO-H563ZI, PA6 reaches only the ST morpho CN12-13, whose headers are not
 * soldered. The board routes SPI1_MISO to the Zio CN7-12 as PG9 (UM3115
 * Table 21, signal SPI_A_MISO), and PG9 carries the same AF5 (DS14258
 * Table 15). Again the connector position is unchanged - CN7-12 either way -
 * and again the DRIVER already picks the right pin per family. */
#if MCU_ID == 563
#define MISO_PORT     CA_GPIO_PORT_G       /* PG9 - PA6 is off-board on H563ZI */
#define MISO_PIN      CA_GPIO_PIN_9
#define MISO_TXT      "PG9"
#else
#define MISO_PORT     CA_GPIO_PORT_A       /* PA6  master MISO */
#define MISO_PIN      CA_GPIO_PIN_6
#define MISO_TXT      "PA6"
#endif

/* The gate goes HERE, after the per-board lists, not before them: the lists
 * are what decides whether this board is supported, so a #if EX29_OK placed
 * above them would be evaluated while the flag still says 1 and the #undef
 * further down would come too late to matter. Same order as ex60. */
#if EX29_OK

#define S_SCK_PORT    CA_GPIO_PORT_C       /* PC10 slave SCK  */
#define S_SCK_PIN     CA_GPIO_PIN_10
#define S_MISO_PORT   CA_GPIO_PORT_C       /* PC11 slave MISO */
#define S_MISO_PIN    CA_GPIO_PIN_11
#define S_MOSI_PORT   CA_GPIO_PORT_C       /* PC12 slave MOSI */
#define S_MOSI_PIN    CA_GPIO_PIN_12

/* Slave side: PC10/PC11/PC12 are NOT on the Arduino CN7 - they are SDMMC1 pins,
 * brought out on the Zio CN8 and, three in a row, on the ST morpho CN11.
 * Verified in UM2861 Table 16/19 (U575) and UM2407 (H753): both boards agree.
 * The original header of this example said CN7-1/3/5, which is wrong on either
 * board and is what made the "wire" test report three dead jumpers - the wires
 * were on the pins the text named, and those pins are not these signals. */
#define WIRE_TXT_1    "SCK   PA5 CN7-10 ---- PC10 CN8-6"
#define WIRE_TXT_2    "MOSI  " MOSI_TXT " CN7-14 ---- PC12 CN8-10"
#define WIRE_TXT_3    "MISO  PC11 CN8-8 ---- " MISO_TXT " CN7-12"  /* slave drives */

/* ============================================================================
 * SHARED STATE
 * ========================================================================== */

#ifdef EN_SPI_LOOPBACK
static u8 g_rx_loopback[POLL_FRAME_SIZE];
#elif defined(EN_SPI_EX_POLL)
static volatile bool g_slave_ready = false;  /* polling handshake */
static u8 g_rx_poll[POLL_FRAME_SIZE];
#endif

#ifndef EN_SPI_LOOPBACK
static u8  g_tx_buf[SPI_DEF_PKT_SIZE];
#endif
#if !defined(EN_SPI_LOOPBACK) && !defined(EN_SPI_EX_POLL)
static u32 g_frame_count = 0U;
#endif

/* ============================================================================
 * SLAVE IRQ CALLBACK — called by SPI3_IRQHandler via ca_spi_recv_handler(3)
 * ============================================================================ */

#ifndef EN_SPI_EX_POLL
static void slave_irq_cb(u8 spi_num, const u8 *data, u16 len)
{
    /* Pattern verification: data[i] must be == (i & 0xFF) */
    u16 errors = 0U;
    for (u16 i = 0; i < len; i++) {
        if (data[i] != (u8)(i & 0xFFU))
            errors++;
    }
    g_frame_count++;
    if (errors == 0U)
        ca_stdout_printf_n("EX29 IRQ: frame %u OK (%u byte)",
                         (unsigned long)g_frame_count, (unsigned)len);
    else
        ca_stdout_printf_n("EX29 IRQ: frame %u ERRORS=%u",
                         (unsigned long)g_frame_count, (unsigned)errors);
    (void)spi_num;
}
#endif

/* ============================================================================
 * TASK MASTER — SPI1
 * ============================================================================ */

/* Set by the "wire" command to park the SPI tasks. Stopping the peripheral is
 * not enough: ca_spi_hw_xfer() re-enables an SPI it finds with SPE=0 (that is
 * how it recovers from a mode fault), so the very next transmission would take
 * the pins back from the GPIO test. The task has to stop asking. */
static volatile bool g_spi_parked = false;

static u32 task_master(void *arg)
{
    (void)arg;

#ifdef EN_SPI_LOOPBACK
    /* Loopback: PB5(MOSI) → PA6(MISO). No slave. */
    ca_os_sleep(50);
    u32 frame = 0U;

    while (1) {
        /* Parked by "wire": the pins belong to the GPIO test now. */
        while (g_spi_parked) ca_os_sleep(100);
        u8 errors = 0U;
        for (u8 i = 0; i < POLL_FRAME_SIZE; i++) {
            u8 rx = ca_spi_xfer(MASTER_SPI, i);
            g_rx_loopback[i] = rx;
            if (rx != i) errors++;
        }
        frame++;
        if (errors == 0U)
            ca_stdout_printf_n("EX29 LOOPBACK: frame %u OK",
                             (unsigned)frame);
        else
            ca_stdout_printf_n("EX29 LOOPBACK: frame %u ERR=%u",
                             (unsigned)frame, (unsigned)errors);
        ca_os_sleep(500);
    }
#elif defined(EN_SPI_EX_POLL)
    /* Small offset: let the slave task start in polling */
    ca_os_sleep(50);
    u32 frame = 0U;

    while (1) {
        /* Parked by "wire": the pins belong to the GPIO test now. */
        while (g_spi_parked) ca_os_sleep(100);
        /* Wait for slave to signal it is receiving */
        while (!g_slave_ready)
            ca_os_yield();

        /* Fill buffer with pattern */
        for (u8 i = 0; i < POLL_FRAME_SIZE; i++)
            g_tx_buf[i] = i;

        ca_spi_write_buf(MASTER_SPI, g_tx_buf, POLL_FRAME_SIZE);

        frame++;
        ca_stdout_printf_n("EX29 POLL M: frame %u TX OK",
                         (unsigned long)frame);

        ca_os_sleep(500);
    }
#else
    ca_os_sleep(100);
    u32 frame = 0U;

    while (1) {
        /* Parked by "wire": the pins belong to the GPIO test now. */
        while (g_spi_parked) ca_os_sleep(100);
        for (u16 i = 0; i < IRQ_FRAME_SIZE; i++)
            g_tx_buf[i] = (u8)(i & 0xFFU);

        ca_spi_write_buf(MASTER_SPI, g_tx_buf, IRQ_FRAME_SIZE);

        frame++;
        ca_stdout_printf_n("EX29 IRQ M: frame %u TX OK",
                         (unsigned long)frame);

        ca_os_sleep(1000);
    }
#endif
return 0;
}

/* ============================================================================
 * TASK SLAVE POLLING — SPI3 (only if EN_SPI_EX_POLL)
 * ============================================================================ */

#if defined(EN_SPI_EX_POLL) && !defined(EN_SPI_LOOPBACK)
static u32 task_slave_poll(void *arg)
{
    (void)arg;
    u32 frame = 0U;

    while (1) {
        /* Parked by "wire": the pins belong to the GPIO test now. */
        while (g_spi_parked) ca_os_sleep(100);
        /* Signal to master that we are ready */
        g_slave_ready = true;

        /* Polling reception: waits for the master's clock, at most
         * CA_SPI_XFER_TIMEOUT_MS per byte. No byte at all = no master yet. */
        const u16 got = ca_spi_read_buf(SLAVE_SPI, g_rx_poll, POLL_FRAME_SIZE);

        g_slave_ready = false;
        if (got == 0U) continue;
        if (got < POLL_FRAME_SIZE) {
            ca_stdout_printf_n("EX29 POLL S: short frame %u/%u (master stopped)",
                               (unsigned)got, (unsigned)POLL_FRAME_SIZE);
            continue;
        }

        /* Pattern verification */
        u8 errors = 0U;
        for (u8 i = 0; i < POLL_FRAME_SIZE; i++) {
            if (g_rx_poll[i] != i)
                errors++;
        }
        frame++;
        if (errors == 0U)
            ca_stdout_printf_n("EX29 POLL S: frame %u OK",
                             (unsigned long)frame);
        else
            ca_stdout_printf_n("EX29 POLL S: frame %u ERR=%u",
                             (unsigned long)frame, (unsigned)errors);

        ca_os_sleep(200);
    }
    return 0;
}
#endif

/* ============================================================================
 * HEARTBEAT — proof that plain GPIO works at all
 *
 * The LED sits on a port this example already uses (GPIOC on U575, where the
 * slave pins live too), so a blinking LED means the port clock, ca_gpio_write
 * and the scheduler are all fine and any "wire" failure is about the pins
 * themselves, not about GPIO being broken.
 * ============================================================================ */

static u32 task_heartbeat(void *arg)
{
    (void)arg;
    ca_led_init();

    ca_os_sleep(1500);
    ca_stdout_put_n("EX29: heartbeat on LED 1 - if it blinks, GPIO is fine");
    ca_stdout_put_n("      commands: wire | info");

    while (1) {
        ca_led_toggle(1U);
        ca_os_sleep(500);
    }
    return 0;
}

/* ============================================================================
 * CLI — local to this example
 * ============================================================================ */

#if ENABLE_CLI

/* Digital continuity test of the three jumpers. The SPI pins are in alternate
 * function here, so they are temporarily reconfigured as plain GPIO — the
 * master side push-pull out, the slave side input with a pulldown — and driven
 * low then high. This answers "is the wire there" without depending on the SPI
 * configuration being right, which is the question that actually costs time
 * when nothing arrives.
 *
 * Both peripherals are stopped first with ca_spi_stop(). Reconfiguring a pin
 * as GPIO would already detach the pad from the SPI, but a slave left enabled
 * keeps its receive interrupt armed and would fire the callback on the edges
 * this test generates — the master side toggling SCK looks exactly like a real
 * transfer to it.
 *
 * The pins do NOT go back to alternate function afterwards, so the command
 * reboots rather than leaving a half-configured SPI behind. */
static bool wire_test_one(cstr name,
                          ca_gpio_port_t op, ca_gpio_pin_t opin,
                          ca_gpio_port_t ip, ca_gpio_pin_t ipin)
{
    ca_gpio_configure(op, opin, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
    ca_gpio_configure(ip, ipin, CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_PULLDOWN, 0);

    ca_gpio_write(op, opin, 0);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    u8 lo = ca_gpio_read(ip, ipin);
    ca_gpio_write(op, opin, 1);
    for (volatile u32 d = 0; d < 20000U; d++) { }
    u8 hi = ca_gpio_read(ip, ipin);

    bool ok = (lo == 0U && hi == 1U);
    CLI_PRINTF_N("  %-34s drive0->read%u drive1->read%u  %s",
                 name, (u32)lo, (u32)hi, ok ? "OK" : "FAIL");
    return ok;
}

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("ex29      : wire (test the 3 jumpers) | starve | info");
        return false;
    }
    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PUT_N("--- Example 29: SPI1 master <-> SPI3 slave, same MCU ---");
        CLI_PUT_N("  " WIRE_TXT_1);
        CLI_PUT_N("  " WIRE_TXT_2);
        CLI_PUT_N("  " WIRE_TXT_3);
        CLI_PUT_N("  led   LED 1 (pin from objmap.h)");
        return true;
    }
    if (ca_strncmp(p, "starve", 6) == 0) {
        /* The slave waiting for a master that never clocks - no jumpers, or
         * the master parked as here: each byte must give up after
         * CA_SPI_XFER_TIMEOUT_MS instead of holding the task for good, and
         * the slave must still work for the next call. No wiring needed. */
        g_spi_parked = true;
        ca_os_sleep(1200);                 /* the master ends its cycle */
        u8 buf[2];
        for (u32 k = 1U; k <= 2U; k++) {
            const u32 t0  = ca_os_tick_ms();
            const u16 got = ca_spi_read_buf(SLAVE_SPI, buf, sizeof(buf));
            CLI_PRINTF_N("EX29 STARVE: try %u - slave got %u/%u bytes in %u ms "
                         "(limit %u ms per byte)", k, (u32)got, (u32)sizeof(buf),
                         ca_os_tick_ms() - t0, (u32)CA_SPI_XFER_TIMEOUT_MS);
        }
        g_spi_parked = false;
        return true;
    }
    if (ca_strncmp(p, "wire", 4) == 0) {
        CLI_PUT_N("--- EX29 wire: continuity of the 3 jumpers ---");
        CLI_PUT_N("  master pin driven, slave pin read (pulldown)");

        /* Park both peripherals: the slave's RX interrupt would otherwise fire
         * on the edges this test generates. Report a failure here rather than
         * letting it look like missing jumpers - if the peripheral does not
         * stop, the pins never leave alternate function and every line below
         * reads 0, which is exactly what a disconnected wire looks like. */
        /* Park the TASKS first, then the peripherals. Stopping only the
         * peripheral is not enough and was the second half of this bug: the
         * master task kept running, ca_spi_hw_xfer() found SPE=0, re-enabled
         * the SPI (its mode-fault recovery path) and took the pins straight
         * back from the GPIO test - the console showed "frame 13 TX OK"
         * arriving after the wire command had started.
         * The sleep covers the task's own 500-1000 ms cycle: it has to reach
         * the top of its loop and see the flag before we touch the pins. */
        g_spi_parked = true;
        CLI_PUT_N("  parking SPI tasks...");
        ca_os_sleep(1200);

        bool m_ok = ca_spi_stop(MASTER_SPI);
        bool s_ok = true;
#ifndef EN_SPI_LOOPBACK
        s_ok = ca_spi_stop(SLAVE_SPI);
#endif
        if (!m_ok || !s_ok)
            CLI_PRINTF_N("  WARNING: spi_stop master=%u slave=%u - results below unreliable",
                         (u32)m_ok, (u32)s_ok);
        /* ca_gpio_configure() does not enable the port clock — ca_gpio_init_port()
         * does, and the SPI driver only set up its own pins. Without this the
         * writes land nowhere and a good jumper reads as missing (the trap that
         * cost time in ex27). */
        ca_gpio_init_port(CA_GPIO_PORT_A);
        ca_gpio_init_port(CA_GPIO_PORT_B);
        ca_gpio_init_port(CA_GPIO_PORT_C);

        u32 bad = 0;
        /* Each line is driven from the end that drives it in normal operation:
         * master -> slave for SCK and MOSI, slave -> master for MISO.
         *
         * The wire itself is symmetric, so it is tempting to drive one fixed
         * side for all three - that is what this did until 2026-08-28, and it
         * reported MISO as FAIL (drive1->read0) on a board whose three jumpers
         * were all present and passing 128-byte frames. Driving the master's
         * MISO pad means driving a pin the SPI holds as an input, against the
         * slave's MISO pad, which the enabled peripheral holds as an output:
         * two drivers on one net, and the reading says nothing about the wire.
         * Continuity is symmetric; the pads at its ends are not. */
        if (!wire_test_one(WIRE_TXT_1, SCK_PORT,  SCK_PIN,  S_SCK_PORT,  S_SCK_PIN))  bad++;
        if (!wire_test_one(WIRE_TXT_2, MOSI_PORT, MOSI_PIN, S_MOSI_PORT, S_MOSI_PIN)) bad++;
        if (!wire_test_one(WIRE_TXT_3, S_MISO_PORT, S_MISO_PIN, MISO_PORT, MISO_PIN)) bad++;

        CLI_PUT_N("  ----------------------------------------------");
        /* One line the bench can read, identical in every example that has a
         * `wire` command - see the note in ex64. The wording below said
         * "RESULT: PASS/ERROR" and counted the BAD ones, which is the opposite
         * convention from the others; the fixed line counts the good. */
        CLI_PRINTF_N("  WIRE RESULT : %s  (%u/3)",
                     (bad == 0U) ? "OK" : "FAIL", (u32)(3U - bad));
        if (bad != 0U)
            CLI_PRINTF_N("  (%u of 3 missing or wrong)", bad);

        CLI_PUT_N("  pins are plain GPIO now - rebooting in 2.5 s...");
        ca_os_sleep(2500);
        ca_os_reset();
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

#endif /* ENABLE_CLI */

/* ============================================================================
 * caOS HOOK
 * ============================================================================ */

void ca_os_configure(void)
{
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
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    /* Init SPI1 — MASTER */
    ca_spi_config_t m_cfg = {
        .spi_num    = MASTER_SPI,
        .br         = CA_SPI_BR_DIV64,   /* APB2/64 ≈ 1.87 MHz on H753 @480MHz/4 */
        .cpol       = 0,
        .cpha       = 0,
        .master     = true,
        .frame_size = 0,
        .on_rx      = NULL,
    };
    ca_spi_init(&m_cfg);

#ifndef EN_SPI_LOOPBACK
#ifdef EN_SPI_EX_POLL
    /* Init SPI3 — SLAVE polling */
    ca_spi_config_t s_cfg = {
        .spi_num    = SLAVE_SPI,
        .br         = 0,              /* slave: baud ignored */
        .cpol       = 0,
        .cpha       = 0,
        .master     = false,
        .frame_size = POLL_FRAME_SIZE,
        .on_rx      = NULL,           /* NULL = polling */
    };
#else
    /* Init SPI3 — SLAVE IRQ */
    ca_spi_config_t s_cfg = {
        .spi_num    = SLAVE_SPI,
        .br         = 0,
        .cpol       = 0,
        .cpha       = 0,
        .master     = false,
        .frame_size = IRQ_FRAME_SIZE,
        .on_rx      = slave_irq_cb,   /* non-NULL = IRQ mode */
    };
#endif
    ca_spi_init(&s_cfg);
#endif /* !EN_SPI_LOOPBACK */
}

void ca_os_main(void)
{
    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    /* Always on, SPI or not: it is the proof that plain GPIO works. */
    ca_os_job("beat", CA_TH_LEVEL3, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("master", CA_TH_LEVEL3, task_master, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

#if defined(EN_SPI_EX_POLL) && !defined(EN_SPI_LOOPBACK)
    ca_os_job("slave",  CA_TH_LEVEL3, task_slave_poll, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
#endif
    /* In loopback/IRQ mode the slave task does not exist */

    ca_os_run(true);
}

#else /* EX29_OK == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m29a, "=== Example 29 ===");
PRIVATE_STRING(m29b, "no SPI, or CN7-14 not characterised for this board");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m29a);
    PUT_N(&priv_m29b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EX29_OK */
