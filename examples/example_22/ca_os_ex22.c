// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 22 — I2C master/slave intra-MCU
 *
 * Hardware (Nucleo-144: H753ZI MB1364, U575ZI-Q MB1549, H563ZI MB1404):
 *   H753:  I2C1 master  PB8=SCL  PB9=SDA   (Zio CN7 pin 2/4, D15/D14)
 *   U575:  I2C4 master  PF14=SCL PF15=SDA  (Zio CN10 pin 8/12, D4/D2)
 *          I2C1 is NOT usable there: it lives on PB6/PB7 and PB7 is the blue
 *          LD2, driven by a transistor that would load the SDA line.
 *   H563:  I2C1 master  PB8=SCL  PB9=SDA   (Zio CN7 pin 2/4, D15/D14)
 *          Same pins as the H753. NOT PB6/PB7, which the RM also allows but
 *          this board wires to the LPUART1 VCP.
 *   H753:  I2C2 slave   PB10=SCL PB11=SDA  (Zio CN10 pin 32/34, D36/D35)
 *   U575:  I2C2 slave   PF1=SCL  PF0=SDA   (Zio CN9 pin 19/21, D69/D68)
 *          PB10/PB11 are NOT usable there: SB61/SB62 are ON by default and
 *          tie PB10 to OCTOSPI_CLK / the motor-control timer.
 *   H563:  I2C2 slave   PF1=SCL  PF0=SDA   (Zio CN9 pin 19/21, D69/D68)
 *          Same pins as the U575. PB10/PB11 are NOT usable: PB10 is CN10-32
 *          as TIM2_CH3 and PB11 reaches no Zio connector at all.
 *   Slave address 0x55
 *
 * Wiring (2 wires + 4.7kΩ pull-up on SDA and SCL) — "show" prints the pins
 * of the board it is running on:
 *   H753:  PB8 ──── PB10   |   PB9  ──── PB11
 *   U575:  PF14 ─── PF1    |   PF15 ─── PF0
 *   H563:  PB8 ──── PF1    |   PB9  ──── PF0
 *
 * The H563 pins were found the hard way: the first attempt used the reference
 * manual defaults and "wire" failed on both lines, because the RM says which
 * pins CAN carry I2C while the board manual says which ones are REACHABLE.
 * Run "wire" first on any new board - it drives the pins as plain GPIO, so it
 * separates a wiring problem from a peripheral one.
 *
 * Protocol (4 fixed bytes):
 *   Master → Slave:   [CMD][D0][D1][D2]
 *   Slave  → Master:  [CMD|0x80][~D0][~D1][cnt_lo]
 *
 * Commands:
 *   CMD_PING  0x01  — connection test
 *   CMD_ECHO  0x02  — slave returns inverted data
 *
 * CLI (debug UART - the console is on UART here, not USB CDC):
 *   ping  — sends CMD_PING and prints response
 *   echo  — sends CMD_ECHO with fixed data and verifies
 *   show  — master + slave statistics
 *   led   — info | on <n> | off <n> | tog <n>   (EN_CLI_LED)
 *   help  — command list
 *
 * LEDs: LED1 toggles on every packet the MASTER sends, LED2 on every packet
 *       the SLAVE receives. With traffic flowing both blink together at the
 *       2 s task period; LED1 alone means the slave is not answering.
 *
 * Note: rx_cb is called in ISR context — only atomic operations,
 *       no sleep, volatile buffers.
 */

#include "caos.h"

#if MCU_ID == 575 || MCU_ID == 753 || MCU_ID == 563
/* Defined in hw/<family>/drivers/i2c_<family>.c — transfer diagnostics.
 * Only the I2C v2 families that implement them: u5xx, h7xx and h5xx. */
#define EX22_DIAG 1
extern u32 ca_i2c_hw_last_error(u32 *stage);
extern u32 ca_i2c_hw_slave_probe(u32 *oar1, u32 *cr1, u32 *isr);
#endif

/* "wire" is a CONTINUITY test: it drives the pins as plain GPIO and never
 * touches the I2C peripheral, so it must NOT hang off EX22_DIAG - that flag
 * gates the I2C v2 diagnostic externs, which are a different thing. Tying the
 * two together is why "wire" answered "huh!" on the F446, the one board that
 * needs it most: its driver defaults land on a LED pin. Enabled wherever the
 * pin map below is board-specific. */
#if MCU_ID == 575 || MCU_ID == 753 || MCU_ID == 563 || MCU_ID == 446 || MCU_ID == 767
#define EX22_WIRE 1
#endif

#if EN_I2C

/* ============================================================================
 * PER-MCU MAPPING — I2C instances used as master and slave
 *
 * Only this block changes between boards; the body below uses the labels.
 * The pins are NOT set here: ca_i2c_hw_configure_gpio() in
 * hw/<family>/drivers/i2c_<family>.c programs them for each instance number.
 * They are repeated in the comments only to describe the wiring.
 * ========================================================================== */

#if MCU_ID == 753
/* NUCLEO-H753ZI (MB1364): I2C1 PB8/PB9 (Arduino CN10 D15/D14),
 *                         I2C2 PB10/PB11 (Zio CN10 D36/D35). */
#define I2C_MASTER      1U
#define I2C_SLAVE       2U
#define I2C_MASTER_ALT  0U   /* H753: default pin map is already PB8/PB9 */
#define I2C_SLAVE_ALT   0U
#define MASTER_TXT      "I2C1 SCL:PB8  CN7-2   SDA:PB9  CN7-4"
#define SLAVE_TXT       "I2C2 SCL:PB10 CN10-32 SDA:PB11 CN10-34"
#define WIRE_SCL        "PB8 CN7-2 (master SCL) ---- PB10 CN10-32 (slave SCL)"
#define WIRE_SDA        "PB9 CN7-4 (master SDA) ---- PB11 CN10-34 (slave SDA)"

#elif MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549) — the master canNOT be I2C1 here: on this part
 * I2C1 is PB6/PB7 (DS13737 Table 27, AF4; PB8/PB9 carry no I2C1 at all), and
 * PB7 is the blue LD2, driven through a transistor (UM2861 §6.4). A LED driver
 * sitting on an open-drain, pulled-up, bidirectional SDA line makes I2C1
 * unusable — this is a wiring fact, not a cosmetic one.
 * I2C4 is used instead: PF14/PF15 AF4, both free on the Zio CN10 (pin 8 = D4
 * and pin 12 = D2, listed as plain "IO" in UM2861 Table 18).
 * The slave keeps I2C2 on PB10/PB11 AF4, same as the H753. */
#define I2C_MASTER      4U
#define I2C_SLAVE       2U
#define I2C_MASTER_ALT  0U   /* U575: I2C4/I2C2 defaults are the right pins */
#define I2C_SLAVE_ALT   0U
#define MASTER_TXT      "I2C4 SCL:PF14 CN10-8  SDA:PF15 CN10-12"
#define SLAVE_TXT       "I2C2 SCL:PF1  CN9-19  SDA:PF0  CN9-21"
#define WIRE_SCL        "PF14 CN10-8  (master SCL) ---- PF1 CN9-19 (slave SCL)"
#define WIRE_SDA        "PF15 CN10-12 (master SDA) ---- PF0 CN9-21 (slave SDA)"

#elif MCU_ID == 563
/* NUCLEO-H563ZI (MB1404), pins verified against UM3115 Tables 21 and 23.
 * The first attempt used the reference-manual defaults PB6/PB7 and PB10/PB11
 * and "wire" failed on both lines: on THIS board PB6/PB7 are the LPUART1 VCP
 * pair, PB10 is CN10-32 as TIM2_CH3, and PB11 never reaches a Zio connector
 * (CN12 morpho only). The pins below are the ones the board actually brings
 * out as I2C. */
#define I2C_MASTER      1U
#define I2C_SLAVE       2U
#define I2C_MASTER_ALT  0U   /* H563: default pin map is already correct */
#define I2C_SLAVE_ALT   0U
#define MASTER_TXT      "I2C1 SCL:PB8 CN7-2   SDA:PB9 CN7-4"
#define SLAVE_TXT       "I2C2 SCL:PF1 CN9-19  SDA:PF0 CN9-21"
#define WIRE_SCL        "PB8 CN7-2 (master SCL) ---- PF1 CN9-19 (slave SCL)"
#define WIRE_SDA        "PB9 CN7-4 (master SDA) ---- PF0 CN9-21 (slave SDA)"

#elif MCU_ID == 446 || MCU_ID == 767
/* NUCLEO-F446ZE and NUCLEO-F767ZI, both MB1137: same connector layout, so the
 * pins verified against UM1974 Table 18 hold for either part.
 * Same trap as the H563: the F4 driver defaults to I2C1 on PB6/PB7, but on
 * THIS board PB7 is the blue LD2 (UM1974 §7.5) and PB6 is QSPI_BK1/D27, while
 * PB11 carries no connector pin at all. Table 18 brings I2C1 out on PB8/PB9
 * instead (CN7-2 "I2C_A_SCL" / CN7-4 "I2C_A_SDA"), and the free I2C pair for
 * the slave is PF1/PF0 on CN9 (D69 "I2C_B_SCL" / D68 "I2C_B_SDA").
 * ⚠️ ca_i2c_hw_configure_gpio() in i2c_f4xx.c still programs PB6/PB7 for
 * I2C1 and PB10/PB11 for I2C2: run "wire" before trusting a transfer here. */
#define I2C_MASTER      1U
#define I2C_SLAVE       2U
/* Both lines need the ALTERNATE map here: the F4 defaults put I2C1 SDA on the
 * blue LD2 (PB7) and I2C2 SDA on PB11, which reaches no connector. */
#define I2C_MASTER_ALT  1U   /* I2C1 -> PB8/PB9  */
#define I2C_SLAVE_ALT   1U   /* I2C2 -> PF1/PF0  */
#define MASTER_TXT      "I2C1 SCL:PB8 CN7-2   SDA:PB9 CN7-4"
#define SLAVE_TXT       "I2C2 SCL:PF1 CN9-19  SDA:PF0 CN9-21"
#define WIRE_SCL        "PB8 CN7-2 (master SCL) ---- PF1 CN9-19 (slave SCL)"
#define WIRE_SDA        "PB9 CN7-4 (master SDA) ---- PF0 CN9-21 (slave SDA)"

#else
/* Other families: keep the driver default (I2C1 master, I2C2 slave). The pins
 * are whatever ca_i2c_hw_configure_gpio() programs for this family — check
 * hw/<family>/drivers/i2c_<family>.c before wiring the board. */
#define I2C_MASTER      1U
#define I2C_SLAVE       2U
#define I2C_MASTER_ALT  0U
#define I2C_SLAVE_ALT   0U
#define MASTER_TXT      "I2C1 (see i2c_<family>.c for the pins)"
#define SLAVE_TXT       "I2C2 (see i2c_<family>.c for the pins)"
#define WIRE_SCL        "master SCL ---- slave SCL"
#define WIRE_SDA        "master SDA ---- slave SDA"
#endif

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */

#define SLAVE_ADDR      0x55u
#define PKT_SIZE        4u

#define CMD_PING        0x01u
#define CMD_ECHO        0x02u
#define RESP_FLAG       0x80u

/* ============================================================================
 * SHARED STATE
 * ========================================================================== */

static volatile u32  g_slave_rx_count  = 0;
static volatile u32  g_master_tx_count = 0;
static volatile u32  g_master_rx_count = 0;
static volatile u32  g_err_count       = 0;
static bool          g_master_up       = false;  /* ca_i2c_init() outcome */
static bool          g_slave_up        = false;  /* ca_i2c_slave_init() outcome */
static volatile bool g_stuck_busy      = false;  /* 'stuck' owns the master pins */

/* ============================================================================
 * SLAVE RX CALLBACK (ISR context)
 * ========================================================================== */

static void on_slave_rx(u8 num, const u8 *buf, u8 len)
{
    if (len < PKT_SIZE) return;

    u8 resp[PKT_SIZE];
    resp[0] = buf[0] | RESP_FLAG;
    resp[1] = (u8)(~buf[1]);
    resp[2] = (u8)(~buf[2]);
    resp[3] = (u8)(g_slave_rx_count & 0xFFu);

    ca_i2c_slave_set_tx(num, resp, PKT_SIZE);
    g_slave_rx_count++;

    /* LED2 marks a packet received by the SLAVE. Safe here: ca_led_toggle()
     * is a plain GPIO write, no OS call, no blocking - the constraint this
     * callback documents above. */
    EVAL_LED( ca_led_toggle(2); )
}

/* ============================================================================
 * MASTER TASK
 * ========================================================================== */
static u32 task_master(void *arg)
{
    (void)arg;
    u8 tx[PKT_SIZE];
    u8 rx[PKT_SIZE];
    u8 seq = 0;

    ca_os_sleep(500);   /* allow time for slave init */

    while (1) {
        u8 cmd = ((seq & 1u) == 0u) ? CMD_PING : CMD_ECHO;
        tx[0] = cmd;
        tx[1] = (u8)(seq + 1u);
        tx[2] = (u8)(seq + 2u);
        tx[3] = 0u;

        /* LED1 marks a packet sent by the MASTER. */
        EVAL_LED( ca_led_toggle(1); )
        if (g_stuck_busy) { ca_os_sleep(100); continue; }
        ca_i2c_write_read(I2C_MASTER, SLAVE_ADDR, tx, PKT_SIZE, rx, PKT_SIZE);
        g_master_tx_count++;

        bool ok = (rx[0] == (cmd | RESP_FLAG)) &&
                  (rx[1] == (u8)(~tx[1]))      &&
                  (rx[2] == (u8)(~tx[2]));
        if (ok) g_master_rx_count++;
        else    g_err_count++;

        seq++;
        ca_os_sleep(2000);
    }
    return 0;
}

#ifdef EX22_WIRE
/* ============================================================================
 * STUCK: reproduce a slave holding SDA low, then clear it with the driver's
 * bus recovery (ca_i2c_bus_recovery).
 *
 * What a reset in the middle of a read leaves behind: the master stops
 * clocking while the slave is driving a 0 bit of its reply. Here the master
 * pins are taken as GPIO and the read is bit-banged up to that point - START,
 * address+R, ACK, then data bits until the slave drives SDA low - and the
 * pins are released as a resetting MCU would. A 'ping' first makes the reply
 * known (0x81..: the second bit is a 0). The driver then has to give the bus
 * back: SDA high, and a normal ping answering again.
 * ========================================================================== */
#if MCU_ID == 575
#define M_SCL_P CA_GPIO_PORT_F
#define M_SCL_N CA_GPIO_PIN_14
#define M_SDA_P CA_GPIO_PORT_F
#define M_SDA_N CA_GPIO_PIN_15
#else   /* H753, H563, F446, F767: I2C1 on PB8/PB9 */
#define M_SCL_P CA_GPIO_PORT_B
#define M_SCL_N CA_GPIO_PIN_8
#define M_SDA_P CA_GPIO_PORT_B
#define M_SDA_N CA_GPIO_PIN_9
#endif

static void bb_wait(void) { for (volatile u32 d = 0; d < 2000U; d++) { } }
static void bb_scl(u8 v) { ca_gpio_write(M_SCL_P, M_SCL_N, v); bb_wait(); }
static void bb_sda(u8 v) { ca_gpio_write(M_SDA_P, M_SDA_N, v); bb_wait(); }

static bool ping_ok(void) {
    u8 tx[PKT_SIZE] = { CMD_PING, 0xAAu, 0x55u, 0u };
    u8 rx[PKT_SIZE] = { 0 };
    ca_i2c_write_read(I2C_MASTER, SLAVE_ADDR, tx, PKT_SIZE, rx, PKT_SIZE);
    return rx[0] == (CMD_PING | RESP_FLAG);
}

static void cmd_stuck(void) {
    g_stuck_busy = true;
    ca_os_sleep(50);                         /* the master task is out of the bus */
    const bool before = ping_ok();          /* also loads the reply 0x81 .. */

    ca_gpio_write(M_SCL_P, M_SCL_N, 1U);
    ca_gpio_write(M_SDA_P, M_SDA_N, 1U);
    ca_gpio_configure(M_SCL_P, M_SCL_N, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_OD,
                      CA_GPIO_SPEED_LOW, CA_GPIO_PULLUP, 0);
    ca_gpio_configure(M_SDA_P, M_SDA_N, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_OD,
                      CA_GPIO_SPEED_LOW, CA_GPIO_PULLUP, 0);
    bb_wait();
    bb_sda(0U);                              /* START */
    bb_scl(0U);
    const u8 a = (u8)((SLAVE_ADDR << 1) | 1U);
    for (i32 b = 7; b >= 0; b--) {
        bb_sda((u8)((a >> b) & 1U));
        bb_scl(1U);
        bb_scl(0U);
    }
    bb_sda(1U);                              /* ACK slot: the slave pulls */
    bb_scl(1U);
    const u8 ack = ca_gpio_read(M_SDA_P, M_SDA_N);
    bb_scl(0U);
    u32 bits = 0U;                           /* clock data until the slave drives a 0 */
    while (bits < 8U && ca_gpio_read(M_SDA_P, M_SDA_N) != 0U) {
        bb_scl(1U);
        bb_scl(0U);
        bits++;
    }
    bb_scl(1U);                              /* "reset": the master lets go */
    const u8 sda_held = (u8)(ca_gpio_read(M_SDA_P, M_SDA_N) == 0U);

    const bool cleared = ca_i2c_bus_recovery(I2C_MASTER);
    const bool after = ping_ok();
    g_stuck_busy = false;

    CLI_PRINTF_N("  stuck      : ping before=%s, addr ACK=%s, SDA held low by the slave "
                 "after %u data bit(s)=%s", before ? "OK" : "ERR", ack == 0U ? "yes" : "NO",
                 bits, sda_held ? "yes" : "NO");
    CLI_PRINTF_N("  recovery   : bus idle=%s, ping after=%s", cleared ? "yes" : "NO",
                 after ? "OK" : "ERR");
    const bool ok = before && ack == 0U && sda_held && cleared && after;
    CLI_PRINTF_N("RUN RESULT : %s  (%u/1)", ok ? "OK" : "FAIL", ok ? 1U : 0U);
}
#endif

/* ============================================================================
 * CLI
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
#ifdef EX22_WIRE
    if (ca_strncmp(p, "wire", 4) == 0) {
        /* Continuity test, no I2C involved: drive each master pin as a plain
         * push-pull output and read the slave pin it should be wired to.
         * This separates "the jumper is not carrying the signal" from every
         * possible peripheral misconfiguration. */
        static const struct { ca_gpio_port_t mp; ca_gpio_pin_t mpin;
                              ca_gpio_port_t sp; ca_gpio_pin_t spin; const char *name; }
        L[] = {
/* The label carries the CONNECTOR pins, not just the port pins: this line is
 * read with a jumper wire in hand, and PB8 says nothing about where to plug
 * it. Numbers from UM2407 (H753), UM2861 (U575) and UM3115 (H563). */
#if MCU_ID == 575
            { CA_GPIO_PORT_F, CA_GPIO_PIN_14, CA_GPIO_PORT_F, CA_GPIO_PIN_1,
              "SCL PF14 CN10-8  -> PF1 CN9-19" },
            { CA_GPIO_PORT_F, CA_GPIO_PIN_15, CA_GPIO_PORT_F, CA_GPIO_PIN_0,
              "SDA PF15 CN10-12 -> PF0 CN9-21" },
#elif MCU_ID == 563
            /* H563: I2C1 PB8/PB9 (CN7) -> I2C2 PF1/PF0 (CN9), per UM3115. */
            { CA_GPIO_PORT_B, CA_GPIO_PIN_8, CA_GPIO_PORT_F, CA_GPIO_PIN_1,
              "SCL PB8 CN7-2 -> PF1 CN9-19" },
            { CA_GPIO_PORT_B, CA_GPIO_PIN_9, CA_GPIO_PORT_F, CA_GPIO_PIN_0,
              "SDA PB9 CN7-4 -> PF0 CN9-21" },
#elif MCU_ID == 446 || MCU_ID == 767
            /* F446 and F767: same layout as the H563, per UM1974 Table 18 -
               both are MB1137 boards. */
            { CA_GPIO_PORT_B, CA_GPIO_PIN_8, CA_GPIO_PORT_F, CA_GPIO_PIN_1,
              "SCL PB8 CN7-2 -> PF1 CN9-19" },
            { CA_GPIO_PORT_B, CA_GPIO_PIN_9, CA_GPIO_PORT_F, CA_GPIO_PIN_0,
              "SDA PB9 CN7-4 -> PF0 CN9-21" },
#else   /* H753: I2C1 PB8/PB9 -> I2C2 PB10/PB11 */
            { CA_GPIO_PORT_B, CA_GPIO_PIN_8, CA_GPIO_PORT_B, CA_GPIO_PIN_10,
              "SCL PB8 CN7-2 -> PB10 CN10-32" },
            { CA_GPIO_PORT_B, CA_GPIO_PIN_9, CA_GPIO_PORT_B, CA_GPIO_PIN_11,
              "SDA PB9 CN7-4 -> PB11 CN10-34" },
#endif
        };
        u32 good = 0U;
        for (u32 i = 0; i < 2; i++) {
            ca_gpio_configure(L[i].mp, L[i].mpin, CA_GPIO_MODE_OUTPUT, CA_GPIO_OTYPE_PP,
                              CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
            ca_gpio_configure(L[i].sp, L[i].spin, CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_PP,
                              CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
            ca_gpio_write(L[i].mp, L[i].mpin, 0);
            for (volatile u32 d = 0; d < 20000; d++) { }
            u8 lo = ca_gpio_read(L[i].sp, L[i].spin);
            ca_gpio_write(L[i].mp, L[i].mpin, 1);
            for (volatile u32 d = 0; d < 20000; d++) { }
            u8 hi = ca_gpio_read(L[i].sp, L[i].spin);
            CLI_PRINTF_N("  %s : drive0->read%u  drive1->read%u  %s",
                         L[i].name, lo, hi,
                         (lo == 0 && hi == 1) ? "OK (wire present)"
                                              : "FAIL (wire missing or swapped)");
            if (lo == 0 && hi == 1) good++;
        }
        /* One line the bench can read, identical in every example that has a
         * `wire` command - see the note in ex64. */
        CLI_PRINTF_N("  WIRE RESULT : %s  (%u/2)",
                     (good == 2U) ? "OK" : "FAIL", good);
        /* Release the pins: leaving them as push-pull outputs driving high
         * would keep an open-drain bus locked for whoever runs next. */
        for (u32 i = 0; i < 2; i++) {
            ca_gpio_configure(L[i].mp, L[i].mpin, CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_OD,
                              CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
            ca_gpio_configure(L[i].sp, L[i].spin, CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_OD,
                              CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
        }
        /* The pins are plain GPIO now, not I2C: reboot rather than asking for
         * the button. The delay leaves the result on screen. */
        CLI_PUT_N("  rebooting in 2.5 s to restore I2C...");
        ca_os_sleep(2500);
        ca_os_reset();
        return true;
    }
#endif
    if (ca_strncmp(p, "help", 4) == 0) {
        /* "led" is not listed here: ca_led_init() registers the LED node,
         * which prints its own help line. */
        CLI_PUT_N("ping | echo | show | wire | stuck");
        return false;
    }
#ifdef EX22_WIRE
    if (ca_strncmp(p, "stuck", 5) == 0) {
        cmd_stuck();
        return true;
    }
#endif
    if (ca_strncmp(p, "ping", 4) == 0) {
        u8 tx[PKT_SIZE] = { CMD_PING, 0xAAu, 0x55u, 0u };
        u8 rx[PKT_SIZE] = { 0 };
        ca_i2c_write_read(I2C_MASTER, SLAVE_ADDR, tx, PKT_SIZE, rx, PKT_SIZE);
        bool ok = (rx[0] == (CMD_PING | RESP_FLAG));
        CLI_PRINTF_N("ping: %02X %02X %02X %02X  %s",
                     (u32)rx[0], (u32)rx[1], (u32)rx[2], (u32)rx[3], ok ? "OK" : "ERR");
        return true;
    }
    if (ca_strncmp(p, "echo", 4) == 0) {
        u8 tx[PKT_SIZE] = { CMD_ECHO, 0x12u, 0x34u, 0u };
        u8 rx[PKT_SIZE] = { 0 };
        ca_i2c_write_read(I2C_MASTER, SLAVE_ADDR, tx, PKT_SIZE, rx, PKT_SIZE);
        bool ok = (rx[0] == (CMD_ECHO | RESP_FLAG)) &&
                  (rx[1] == (u8)(~tx[1]))            &&
                  (rx[2] == (u8)(~tx[2]));
        CLI_PRINTF_N("echo: %02X %02X %02X %02X  %s",
                     (u32)rx[0], (u32)rx[1], (u32)rx[2], (u32)rx[3], ok ? "OK" : "ERR");
        return true;
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        CLI_PUT_N("--- Example 22 I2C master/slave ---");
        CLI_PUT_N("  master     : " MASTER_TXT);
        CLI_PUT_N("  slave      : " SLAVE_TXT);
        CLI_PUT_N("  wiring     : " WIRE_SCL);
        CLI_PUT_N("               " WIRE_SDA);
        CLI_PRINTF_N("  i2c init   : master=%s  slave=%s",
                     g_master_up ? "OK" : "FAILED", g_slave_up ? "OK" : "FAILED");
#ifdef EX22_DIAG
        {   /* Why the last transfer failed — no scope needed. */
            u32 stage = 0;
            u32 isr   = ca_i2c_hw_last_error(&stage);
            static const char *st[] = { "-", "TXIS", "TC", "RXNE", "BUSY-stuck" };
            CLI_PRINTF_N("  last fail  : stage=%s  ISR=0x%08X",
                         st[stage <= 4U ? stage : 0U], isr);
            CLI_PRINTF_N("    BUSY=%u NACKF=%u BERR=%u ARLO=%u TXIS=%u RXNE=%u",
                         (isr >> 15) & 1U, (isr >> 4) & 1U, (isr >> 8) & 1U,
                         (isr >> 9) & 1U, (isr >> 1) & 1U, (isr >> 2) & 1U);
            u32 oar1 = 0, cr1 = 0, sisr = 0;
            u32 nirq = ca_i2c_hw_slave_probe(&oar1, &cr1, &sisr);
            CLI_PRINTF_N("  slave regs : irq=%u OAR1=0x%08X CR1=0x%08X ISR=0x%08X",
                         nirq, oar1, cr1, sisr);
            CLI_PRINTF_N("    OA1EN=%u addr7=0x%02X  PE=%u ADDRIE=%u",
                         (oar1 >> 15) & 1U, (oar1 >> 1) & 0x7FU,
                         cr1 & 1U, (cr1 >> 3) & 1U);
        }
#endif
        CLI_PRINTF_N("  master tx  : %u", g_master_tx_count);
        CLI_PRINTF_N("  master rx  : %u", g_master_rx_count);
        CLI_PRINTF_N("  errors     : %u", g_err_count);
        CLI_PRINTF_N("  slave rx   : %u", g_slave_rx_count);
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ============================================================================
 * ENTRY POINTS
 * ========================================================================== */

void ca_os_configure(void)
{
    /* Master: polling. Both init functions return bool — report a failure, or
     * the example keeps counting TX attempts against a peripheral that was
     * never brought up (that is how the missing I2C4 clock stayed invisible). */
    g_master_up = ca_i2c_init(&(ca_i2c_config_t){ .i2c_num   = I2C_MASTER,
                                                  .speed     = CA_I2C_SPEED_STANDARD,
                                                  .alternate = I2C_MASTER_ALT });

    /* Slave: IRQ-driven */
    g_slave_up = ca_i2c_slave_init(I2C_SLAVE, SLAVE_ADDR, on_slave_rx,
                                   I2C_SLAVE_ALT);

    EVAL_LED( ca_led_init(); )

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
}

void ca_os_main(void)
{
    ca_os_job("master", CA_TH_LEVEL3, task_master, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}

#else  /* EN_I2C == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m22a, "=== Example 22 ===");
PRIVATE_STRING(m22b, "EN_I2C == 0, skipping I2C example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m22a);
    PUT_N(&priv_m22b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_I2C */
