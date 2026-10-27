// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 21 — Multi-instance UART with binary protocol
 *
 * Two UART instances talk to each other over two jumper wires: one acts as
 * MASTER (sends commands), the other as SLAVE (answers). The console/CLI runs
 * on a third, separate UART. Board-specific pins, LEDs and UART numbers are
 * defined in the per-MCU block below; the body of the example uses only the
 * labels and is identical on every board.
 *
 * Binary protocol (max 64 bytes):
 *   [SOP=0xAA][CMD 1B][LEN 1B][PAYLOAD 0..60B][CHK 1B]
 *   CHK = sum of all previous bytes (mod 256)
 *   Response: CMD | 0x80
 *
 * Commands:
 *   HELLO  0x10  payload: none
 *                response 0x90: payload none
 *   STATUS 0x20  payload: none
 *                response 0xA0: payload [cmds_rx, uptime_lo, uptime_hi, ok=1]
 *
 * CLI (USB CDC or debug UART):
 *   hello  — send HELLO from the master
 *   status — send STATUS from the master
 *   show   — master + slave statistics
 *   bridge — VCP <-> slave UART repeater: echo self-test, converter, baud
 *            (see BRIDGE below; uses the slave UART, jumpers off)
 *   help   — command list
 */

#include "caos.h"


/* An MCU without a mapping below is not an error: the example simply does not
 * apply to that board, so EX21_OK stays 0 and everything falls through to the
 * "not supported" branch at the bottom. An #error here would instead break the
 * build of every OTHER example in the tree. */
#if ENABLE_UART
#define EX21_OK   1
#else
#define EX21_OK   0
#endif

/* ============================================================================
 * PER-MCU MAPPING — UART instances, pins and LEDs
 *
 * Only this block changes between boards. Pin/AF assignment for each UART
 * number lives in hw/<family>/drivers/uart_<family>.c (ca_uart_hw_configure_gpio):
 * the pins listed here are what that function programs, repeated for wiring.
 * ========================================================================== */

#if MCU_ID == 753
/* NUCLEO-H753ZI (MB1364) — VCP/CLI on USART3 (PD8/PD9), so UART7+UART2 are free.
 * LEDs UM2407 §6.5: LD1 green PB0, LD2 yellow PE1, LD3 red PB14. */
#define UART_MASTER      7U
#define UART_SLAVE       2U
#define UART_MASTER_ALT  0U
#define UART_SLAVE_ALT   0U
#define MASTER_PINS      "UART7  TX:PE8 CN10-18  RX:PE7 CN10-20"
#define SLAVE_PINS       "USART2 TX:PD5 CN9-6    RX:PD6 CN9-4"
#define M_TX_PORT CA_GPIO_PORT_E
#define M_TX_PIN  CA_GPIO_PIN_8
#define M_RX_PORT CA_GPIO_PORT_E
#define M_RX_PIN  CA_GPIO_PIN_7
#define S_TX_PORT CA_GPIO_PORT_D
#define S_TX_PIN  CA_GPIO_PIN_5
#define S_RX_PORT CA_GPIO_PORT_D
#define S_RX_PIN  CA_GPIO_PIN_6
#define WIRE_1           "PE8 CN10-18 (master TX) ---- PD6 CN9-4  (slave RX)"
#define WIRE_2           "PD5 CN9-6   (slave TX)  ---- PE7 CN10-20 (master RX)"

#elif MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549) — no UART7 on this part (LPUART1 + USART1-3 + UART4-5).
 *   USART1 PA9/PA10 -> STLINK-V3E VCP, i.e. our debug console (UART_DBG_NUM=1)
 *   USART2 PD5/PD6  -> "USART_B" on CN9 pin 6/4 (UM2861 Table 17) -> SLAVE
 *   UART4  PA0/PA1  -> AF8, both on CN10 (pin 29 / pin 11)        -> MASTER
 * NOT usable as the master here:
 *   LPUART1 — ca_uart_init() rejects uart_num 0 (uart.c:94), it is reachable
 *             only through the separate ca_lpuart_* API, so it cannot be a
 *             peer of ca_uart_* in this example.
 *   USART3  — the driver maps it to PD8/PD9, which this board routes only as
 *             raw morpho pins on two different connectors (CN12-10, CN11-69).
 * PA0 is D32 on CN10 pin 29 (the same pin ex24 drives on the H753 header) and
 * PA1 is A8 on CN10 pin 11: master TX and RX sit on the same connector.
 * LEDs UM2861 §6.4: LD1 green PC7, LD2 blue PB7, LD3 red PG2. */
#define UART_MASTER      4U
#define UART_SLAVE       2U
#define UART_MASTER_ALT  0U
#define UART_SLAVE_ALT   0U
#define MASTER_PINS      "UART4  TX:PA0 CN10-29  RX:PA1 CN10-11"
#define SLAVE_PINS       "USART2 TX:PD5 CN9-6    RX:PD6 CN9-4"
#define M_TX_PORT CA_GPIO_PORT_A
#define M_TX_PIN  CA_GPIO_PIN_0
#define M_RX_PORT CA_GPIO_PORT_A
#define M_RX_PIN  CA_GPIO_PIN_1
#define S_TX_PORT CA_GPIO_PORT_D
#define S_TX_PIN  CA_GPIO_PIN_5
#define S_RX_PORT CA_GPIO_PORT_D
#define S_RX_PIN  CA_GPIO_PIN_6
#define WIRE_1           "PA0 CN10-29 (master TX) ---- PD6 CN9-4  (slave RX)"
#define WIRE_2           "PD5 CN9-6   (slave TX)  ---- PA1 CN10-11 (master RX)"

#elif MCU_ID == 563
/* NUCLEO-H563ZI (MB1404), pins verified against UM3115. VCP/CLI is USART3
 * (PD8/PD9), so USART6 and USART2 are free.
 *   USART6 PC6/PC7  -> CN7 pin 1 (D16) / pin 11 (D21)   -> MASTER
 *   USART2 PD5/PD6  -> CN9 pin 6 "USART_B_TX" / pin 4 "USART_B_RX" -> SLAVE
 * The driver already maps USART2 to PD5/PD6 on this family, which is exactly
 * the pair the board brings out as USART_B. */
#define UART_MASTER      6U
#define UART_SLAVE       2U
#define UART_MASTER_ALT  0U
#define UART_SLAVE_ALT   0U
#define MASTER_PINS      "USART6 TX:PC6 CN7-1    RX:PC7 CN7-11"
#define SLAVE_PINS       "USART2 TX:PD5 CN9-6    RX:PD6 CN9-4"
#define M_TX_PORT CA_GPIO_PORT_C
#define M_TX_PIN  CA_GPIO_PIN_6
#define M_RX_PORT CA_GPIO_PORT_C
#define M_RX_PIN  CA_GPIO_PIN_7
#define S_TX_PORT CA_GPIO_PORT_D
#define S_TX_PIN  CA_GPIO_PIN_5
#define S_RX_PORT CA_GPIO_PORT_D
#define S_RX_PIN  CA_GPIO_PIN_6
#define WIRE_1           "PC6 CN7-1  (master TX) ---- PD6 CN9-4  (slave RX)"
#define WIRE_2           "PD5 CN9-6  (slave TX)  ---- PC7 CN7-11 (master RX)"

#elif MCU_ID == 446
/* NUCLEO-F446ZE (MB1137), pins verified against UM1974 Table 18. VCP/CLI is
 * USART3 (PD8/PD9), so USART6 and USART2 are free - but NOT on the pins the
 * F4 driver programs for USART2 (PA2/PA3): those two do not appear in Table 18
 * at all, because the board wires them to the ST-LINK VCP. UART4 is out for the
 * same reason - PA1 is missing too, so only PA0 of the pair is reachable.
 *   USART6 PC6/PC7  -> CN7 pin 1 (D16) / pin 11 (D21)   -> MASTER
 *   USART2 PD5/PD6  -> CN9 pin 6 "USART_B_TX" / pin 4 "USART_B_RX" -> SLAVE
 * uart_f4xx.c programs USART2 on PA2/PA3 by default, so the slave asks for pin
 * map 1 (the last argument of ca_uart_init) to move it to PD5/PD6 - the pair
 * UM1974 names "USART_B_TX"/"USART_B_RX", the same one the H563 uses. AF7 for
 * both, confirmed on DS10693 Table 11. USART6 PC6/PC7 is AF8 and the driver
 * already has it right, so the master stays on pin map 0. */
#define UART_MASTER      6U
#define UART_SLAVE       2U
#define UART_MASTER_ALT  0U
#define UART_SLAVE_ALT   1U   /* USART2 -> PD5/PD6 AF7, not PA2/PA3 */
#define MASTER_PINS      "USART6 TX:PC6 CN7-1    RX:PC7 CN7-11"
#define SLAVE_PINS       "USART2 TX:PD5 CN9-6    RX:PD6 CN9-4"
#define M_TX_PORT CA_GPIO_PORT_C
#define M_TX_PIN  CA_GPIO_PIN_6
#define M_RX_PORT CA_GPIO_PORT_C
#define M_RX_PIN  CA_GPIO_PIN_7
#define S_TX_PORT CA_GPIO_PORT_D
#define S_TX_PIN  CA_GPIO_PIN_5
#define S_RX_PORT CA_GPIO_PORT_D
#define S_RX_PIN  CA_GPIO_PIN_6
#define WIRE_1           "PC6 CN7-1  (master TX) ---- PD6 CN9-4  (slave RX)"
#define WIRE_2           "PD5 CN9-6  (slave TX)  ---- PC7 CN7-11 (master RX)"

#elif MCU_ID == 767
/* NUCLEO-F767ZI (MB1137): same board as the F446, same pins - but checked
 * against the documents of the PART, not carried over by analogy.
 *   DS11532 Table 13: PC6=USART6_TX / PC7=USART6_RX in AF8,
 *                     PD5=USART2_TX / PD6=USART2_RX in AF7.
 *   UM1974 Table 19 (the F746ZG/F756ZG/F767ZI row):
 *                     PC6=CN7-1 (D16), PC7=CN7-11 (D21),
 *                     PD6=CN9-4 "USART_B_RX", PD5=CN9-6 "USART_B_TX".
 * The console/CLI is USART3 (PD8/PD9, the ST-LINK VCP), so USART6 and USART2
 * are free. As on the F446, USART2 has to move to PD5/PD6 with pin map 1: the
 * default PA2/PA3 is not routed to the connectors. uart_f7xx.c already has
 * that map (USART2 alt 1 = PD5/PD6 AF7, USART6 = PC6/PC7 AF8). */
#define UART_MASTER      6U
#define UART_SLAVE       2U
#define UART_MASTER_ALT  0U
#define UART_SLAVE_ALT   1U   /* USART2 -> PD5/PD6 AF7, non PA2/PA3 */
#define MASTER_PINS      "USART6 TX:PC6 CN7-1    RX:PC7 CN7-11"
#define SLAVE_PINS       "USART2 TX:PD5 CN9-6    RX:PD6 CN9-4"
#define M_TX_PORT CA_GPIO_PORT_C
#define M_TX_PIN  CA_GPIO_PIN_6
#define M_RX_PORT CA_GPIO_PORT_C
#define M_RX_PIN  CA_GPIO_PIN_7
#define S_TX_PORT CA_GPIO_PORT_D
#define S_TX_PIN  CA_GPIO_PIN_5
#define S_RX_PORT CA_GPIO_PORT_D
#define S_RX_PIN  CA_GPIO_PIN_6
#define WIRE_1           "PC6 CN7-1  (master TX) ---- PD6 CN9-4  (slave RX)"
#define WIRE_2           "PD5 CN9-6  (slave TX)  ---- PC7 CN7-11 (master RX)"

#else
/* No UART mapping for this board: the example does not apply here. Turn it
 * off instead of failing the build - it needs two spare UARTs wired to each
 * other, which is a property of the board, not a defect. */
#undef  EX21_OK
#define EX21_OK  0
#endif

#if EX21_OK
/* ============================================================================
 * PROTOCOL
 * ========================================================================== */

#define PKT_SOP        0xAAu
#define PKT_MAX        64u
#define PKT_HDR        4u                    /* SOP + CMD + LEN + CHK */
#define PKT_MAX_PL     (PKT_MAX - PKT_HDR)   /* 60 byte payload max */

#define CMD_HELLO      0x10u
#define CMD_STATUS     0x20u
#define RESP_FLAG      0x80u

/* ============================================================================
 * STATE MACHINE RX (ISR context — no heap, no blocking)
 * ========================================================================== */

typedef enum { RX_SOP, RX_CMD, RX_LEN, RX_PAYLOAD, RX_CHK } rx_state_t;

typedef struct {
    rx_state_t   state;
    u8           cmd;
    u8           len;
    u8           payload[PKT_MAX_PL];
    u8           pi;          /* current payload index */
    u8           chk;         /* accumulated checksum */
    /* packet completed */
    u8           pkt[PKT_MAX];
    u8           pkt_size;
    volatile bool ready;      /* true = packet ready, consume and reset */
    u32          err;         /* counter of packets with incorrect checksum */
} rx_ctx_t;

static void rx_feed(rx_ctx_t *ctx, u8 byte)
{
    switch (ctx->state) {
        case RX_SOP:
            if (byte == PKT_SOP) {
                ctx->chk   = byte;
                ctx->state = RX_CMD;
            }
            break;
        case RX_CMD:
            ctx->cmd   = byte;
            ctx->chk  += byte;
            ctx->state = RX_LEN;
            break;
        case RX_LEN:
            ctx->len = byte;
            ctx->chk += byte;
            ctx->pi  = 0;
            if (byte == 0)
                ctx->state = RX_CHK;
            else if (byte <= PKT_MAX_PL)
                ctx->state = RX_PAYLOAD;
            else
                ctx->state = RX_SOP;   /* invalid length */
            break;
        case RX_PAYLOAD:
            ctx->payload[ctx->pi++]  = byte;
            ctx->chk                += byte;
            if (ctx->pi == ctx->len)
                ctx->state = RX_CHK;
            break;
        case RX_CHK:
            if (byte == ctx->chk) {
                /* copy complete packet and signal */
                ctx->pkt[0] = PKT_SOP;
                ctx->pkt[1] = ctx->cmd;
                ctx->pkt[2] = ctx->len;
                for (u8 i = 0; i < ctx->len; i++)
                    ctx->pkt[3 + i] = ctx->payload[i];
                ctx->pkt[3 + ctx->len] = byte;
                ctx->pkt_size = PKT_HDR + ctx->len;
                ctx->ready    = true;
            } else {
                ctx->err++;
                ca_led_toggle(3U); /* LD3 red: chk error */
            }
            ctx->state = RX_SOP;
            break;
    }
}

/* ============================================================================
 * RX CONTEXTS
 * ========================================================================== */

static rx_ctx_t g_rx_m;   /* responses from slave  → received on master RX */
static rx_ctx_t g_rx_s;   /* commands from master  → received on slave RX  */

/* Toggling LD1/LD2 on every single received byte is invisible at 115200 baud
 * (a whole packet clears in well under a millisecond): toggle once every 10
 * bytes instead, so activity is visible without slowing down the UART. */
#define RX_LED_EVERY   10U

static void on_rx_master(u32 byte) {
    static u32 count;
    if (++count >= RX_LED_EVERY) { count = 0; ca_led_toggle(1U); } /* LD1: master RX */
    rx_feed(&g_rx_m, (u8)byte);
}
static void on_rx_slave(u32 byte) {
    static u32 count;
    if (++count >= RX_LED_EVERY) { count = 0; ca_led_toggle(2U); } /* LD2: slave RX */
    rx_feed(&g_rx_s, (u8)byte);
}

/* ============================================================================
 * PACKET BUILDER + SEND
 * ========================================================================== */

static void uart_send(u32 num, u8 cmd, const u8 *pl, u8 plen)
{
    string raw_s;
    p_str msg = &raw_s;
    u8   *buf = (u8 *)msg->data;

    buf[0] = PKT_SOP;
    buf[1] = cmd;
    buf[2] = plen;
    u8 chk = buf[0] + buf[1] + buf[2];
    for (u8 i = 0; i < plen; i++) {
        buf[3 + i]  = pl[i];
        chk        += pl[i];
    }
    buf[3 + plen] = chk;
    msg->size     = PKT_HDR + plen;
    ca_uart_put(num, msg);
}

/* ============================================================================
 * STATISTICS
 * ========================================================================== */

static volatile u32 g_tx_hello    = 0;
static volatile u32 g_tx_status   = 0;
static volatile u32 g_rx_hello    = 0;   /* HELLO responses received on master RX */
static volatile u32 g_rx_status   = 0;   /* STATUS responses received on master RX */
static volatile u32 g_slave_cmds  = 0;   /* total commands processed by the slave */
static volatile u32 g_slave_ticks = 0;   /* slave tick (×10ms) */
static bool g_master_up = false;         /* ca_uart_init() outcome, master */
static bool g_slave_up  = false;         /* ca_uart_init() outcome, slave  */
static ca_task_handle_t g_master_task;  /* master (created suspended) - a handle, never a thid */
static ca_task_handle_t g_slave_task;   /* slave  (created suspended) */

/* ============================================================================
 * WIRE TEST — GPIO continuity of the two crossed jumpers, no UART involved,
 * no reboot. Used by the CLI "wire" command and by task_work below.
 * ========================================================================== */

static bool test_wire(void)
{
    static const struct { ca_gpio_port_t op; ca_gpio_pin_t opin;
                          ca_gpio_port_t ip; ca_gpio_pin_t ipin; const char *name; }
    L[] = {
        { M_TX_PORT, M_TX_PIN, S_RX_PORT, S_RX_PIN, WIRE_1 },
        { S_TX_PORT, S_TX_PIN, M_RX_PORT, M_RX_PIN, WIRE_2 },
    };
    bool all_ok = true;
    u32 good = 0U;
    for (u32 i = 0; i < 2U; i++) {
        ca_gpio_configure(L[i].op, L[i].opin, CA_GPIO_MODE_OUTPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
        ca_gpio_configure(L[i].ip, L[i].ipin, CA_GPIO_MODE_INPUT,
                          CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0);
        ca_gpio_write(L[i].op, L[i].opin, 0);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 lo = ca_gpio_read(L[i].ip, L[i].ipin);
        ca_gpio_write(L[i].op, L[i].opin, 1);
        for (volatile u32 d = 0; d < 20000U; d++) { }
        u8 hi = ca_gpio_read(L[i].ip, L[i].ipin);
        bool ok = (lo == 0 && hi == 1);
        if (!ok) all_ok = false; else good++;
        ca_stdout_printf_n("  %s : drive0->read%u  drive1->read%u  %s",
                     L[i].name, lo, hi,
                     ok ? "OK (wire present)" : "FAIL (wire missing or swapped)");
    }
    /* One line the bench can read, identical in every example that has a
     * `wire` command - see the note in ex64. Printed here rather than at the
     * call site because test_wire() is also called by task_work at start-up,
     * and the verdict is worth having in the boot log too. */
    ca_stdout_printf_n("  WIRE RESULT : %s  (%u/2)",
                       all_ok ? "OK" : "FAIL", good);
    return all_ok;
}

/* ============================================================================
 * TASK WORK — orchestrates the boot sequence to isolate the ex21 U575 bug:
 * banner first (task_master/task_slave stay suspended, no UART touched yet),
 * then resume slave and later master — each of those does its own
 * ca_uart_init() on the way in (100ms settle sleep right after). Every step
 * logs where we are — see project_ex21_u575_led_init_kills_stdout.md.
 * ========================================================================== */

static u32 task_work(void *arg)
{
    (void)arg;

    ca_stdout_printf_n("[work] step 1: waiting 2s for the banner to complete...");
    ca_os_sleep(2000);

    ca_stdout_printf_n("[work] step 2: resume slave (ca_uart_init runs inside task_slave)");
    ca_os_resume_handle(g_slave_task);

    ca_os_sleep(3000);

    ca_stdout_printf_n("[work] step 3: resume master (ca_uart_init runs inside task_master)");
    ca_os_resume_handle(g_master_task);

    ca_stdout_printf_n("[work] done");
    return 0;
}

/* ============================================================================
 * TASK MASTER — sends commands on the master UART, processes its responses
 * ========================================================================== */

static u32 task_master(void *arg)
{
    (void)arg;

    /* Brought up from inside the task itself (resumed by task_work), not from
     * ca_os_configure() — see project_ex21_u575_led_init_kills_stdout.md. */
    g_master_up = ca_uart_init(UART_MASTER, CA_UART_BAUD_115200, on_rx_master,
                               UART_MASTER_ALT);
    ca_os_sleep(100);   /* let the UART settle before real traffic */
    ca_os_sleep(200);   /* wait for slave initialization */

    while (1) {
        /* send HELLO from the master, wait for response (max 200ms) */
        uart_send(UART_MASTER, CMD_HELLO, NULL, 0);
        g_tx_hello++;
        for (u32 t = 0; t < 20; t++) {
            ca_os_sleep(10);
            if (g_rx_m.ready) {
                if (g_rx_m.pkt[1] == (CMD_HELLO | RESP_FLAG))
                    g_rx_hello++;
                g_rx_m.ready = false;
                break;
            }
        }
        ca_os_sleep(500);

        /* send STATUS from the master, wait for response (max 200ms) */
        uart_send(UART_MASTER, CMD_STATUS, NULL, 0);
        g_tx_status++;
        for (u32 t = 0; t < 20; t++) {
            ca_os_sleep(10);
            if (g_rx_m.ready) {
                if (g_rx_m.pkt[1] == (CMD_STATUS | RESP_FLAG))
                    g_rx_status++;
                g_rx_m.ready = false;
                break;
            }
        }
        ca_os_sleep(500);
    }
    return 0;
}

/* ============================================================================
 * TASK SLAVE — processes commands from the slave UART RX, responds on its TX
 * ========================================================================== */

static u32 task_slave(void *arg)
{
    (void)arg;

    g_slave_up = ca_uart_init(UART_SLAVE, CA_UART_BAUD_115200, on_rx_slave,
                              UART_SLAVE_ALT);
    ca_os_sleep(100);   /* let the UART settle before real traffic */

    while (1) {
        g_slave_ticks++;

        if (g_rx_s.ready) {
            u8 cmd = g_rx_s.pkt[1];
            g_rx_s.ready = false;
            g_slave_cmds++;

            if (cmd == CMD_HELLO) {
                uart_send(UART_SLAVE, CMD_HELLO | RESP_FLAG, NULL, 0);

            } else if (cmd == CMD_STATUS) {
                u8 pl[4];
                pl[0] = (u8)(g_slave_cmds & 0xFF);
                pl[1] = (u8)(g_slave_ticks & 0xFF);
                pl[2] = (u8)((g_slave_ticks >> 8) & 0xFF);
                pl[3] = 0x01;   /* OK */
                uart_send(UART_SLAVE, CMD_STATUS | RESP_FLAG, pl, 4);
            }
        }
        ca_os_sleep(10);
    }
    return 0;
}
/* ============================================================================
 * BRIDGE — ST-LINK VCP <-> slave UART (USART2: PD5 TX CN9-6, PD6 RX CN9-4 on
 * every Nucleo-144 mapped above)
 *
 * The VCP is the only serial port a bench PC has for free; the bridge repeats
 * it on a UART that reaches the connectors, so a second board's console (e.g.
 * ex62's LPUART) needs no USB-serial adapter. It uses the SLAVE UART: remove
 * the master/slave jumpers first.
 *   bridge echo      - self-test with ONE jumper CN9-6 <-> CN9-4: a pattern
 *                      goes out of TX and must come back on RX. PASS/FAIL.
 *   bridge conv      - converter: from now on every VCP byte goes out of the
 *                      UART TX and every UART RX byte comes back on the VCP.
 *                      The CLI and stdout are gone until RESET (by choice).
 *   bridge baud <n>  - speed of the bridge UART, default 115200 8N1.
 * Both directions go through a ring filled in the RX ISR and drained by a
 * task, never written from the ISR: ca_uart_put() waits on the UART.
 * ========================================================================== */

#define BR_RING   1024U
typedef struct { volatile u16 head, tail; u8 buf[BR_RING]; } br_ring_t;

static br_ring_t s_to_peer;           /* VCP RX         -> bridge UART TX */
static br_ring_t s_to_vcp;            /* bridge UART RX -> VCP TX         */
static u32 s_br_baud = CA_UART_BAUD_115200;
static volatile u32 s_br_drop;        /* bytes lost to a full ring */

static void br_push(br_ring_t *r, u8 b)
{
    const u16 n = (u16)((r->head + 1U) % BR_RING);
    if (n == r->tail) { s_br_drop++; return; }
    r->buf[r->head] = b;
    r->head = n;
}

static u32 br_pop(br_ring_t *r, u8 *dst, u32 max)
{
    u32 n = 0U;
    while (n < max && r->tail != r->head) {
        dst[n++] = r->buf[r->tail];
        r->tail = (u16)((r->tail + 1U) % BR_RING);
    }
    return n;
}

static void on_rx_br_peer(u32 byte) { br_push(&s_to_vcp,  (u8)byte); }
static void on_rx_br_vcp(u32 byte)  { br_push(&s_to_peer, (u8)byte); }

/* Drain one ring into one UART; returns the bytes moved */
static u32 br_pump(br_ring_t *r, u32 uart)
{
    string s;
    const u32 n = br_pop(r, (u8 *)s.data, sizeof(s.data));
    if (n > 0U) {
        s.size = (u16)n;
        ca_uart_put(uart, &s);
    }
    return n;
}

/* stdout is muted in converter mode: anything the board prints would land in
 * the other board's console as if typed there */
static u16 br_null_sender(p_str str) { return str ? str->size : 0U; }

static void bridge_echo(void)
{
    static const char pat[] = "caOS bridge echo 0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n";
    const u32 plen = sizeof(pat) - 1U;
    const u32 rounds = 4U;
    u8 got[sizeof(pat)];
    /* One reading: a 'bridge baud' typed meanwhile (commands run in their own
     * task) must not change the speed in the middle of the test */
    const u32 baud = s_br_baud;

    if (!ca_uart_init(UART_SLAVE, baud, on_rx_br_peer, UART_SLAVE_ALT)) {
        CLI_PUT_N("bridge echo: FAIL (uart init)");
        return;
    }
    ca_os_sleep(20);
    s_to_vcp.tail = s_to_vcp.head;          /* drop anything left on the line */
    s_br_drop = 0U;

    u32 ok = 0U, bad = 0U, lost = 0U;
    for (u32 r = 0U; r < rounds; r++) {
        string s;
        ca_memcpy(s.data, pat, plen);
        s.size = (u16)plen;
        ca_uart_put(UART_SLAVE, &s);
        /* time on the wire + margin: 10 bits per byte */
        ca_os_sleep(1U + (plen * 10U * 1000U) / baud + 20U);
        const u32 n = br_pop(&s_to_vcp, got, plen);
        if (n < plen) lost += plen - n;
        for (u32 i = 0U; i < n; i++) {
            if (got[i] == (u8)pat[i]) ok++; else bad++;
        }
    }
    const bool pass = (ok == plen * rounds && bad == 0U && s_br_drop == 0U);
    CLI_PRINTF_N("bridge echo: %s (%u/%u bytes ok, %u wrong, %u lost) at %u baud",
                 pass ? "PASS" : "FAIL", ok, plen * rounds, bad, lost, baud);
    if (!pass) {
        CLI_PUT_N("  jumper " SLAVE_PINS " : TX to RX on the same UART");
    }
    /* Same verdict line as 'wire': the bench loops on it to ask for the
     * jumper and, at the end, to see it removed */
    CLI_PRINTF_N("  WIRE RESULT : %s  (%u/1)", pass ? "OK" : "FAIL", pass ? 1U : 0U);
    /* Give the UART back to the ex21 slave */
    g_slave_up = ca_uart_init(UART_SLAVE, CA_UART_BAUD_115200, on_rx_slave,
                              UART_SLAVE_ALT);
}

/* Never returns: the converter ends with a RESET */
static void bridge_conv(void)
{
    CLI_PRINTF_N("bridge conv: VCP <-> " SLAVE_PINS " at %u baud 8N1", s_br_baud);
    CLI_PUT_N("bridge conv: CLI and stdout off from now on - RESET to leave");
    ca_os_sleep(200);                        /* let stdout drain those lines */

    if (!ca_uart_init(UART_SLAVE, s_br_baud, on_rx_br_peer, UART_SLAVE_ALT)) {
        CLI_PUT_N("bridge conv: FAIL (uart init)");
        return;
    }
    EVAL_STDOUT( ca_stdout_set_sender(br_null_sender); )
    s_to_peer.tail = s_to_peer.head;
    s_to_vcp.tail  = s_to_vcp.head;
    /* The VCP RX now feeds the bridge instead of the CLI */
    (void)ca_uart_init(UART_DBG_NUM, UART_DBG_BAUDRATE, on_rx_br_vcp, UART_DBG_PINMAP);

    while (1) {
        const u32 moved = br_pump(&s_to_peer, UART_SLAVE) + br_pump(&s_to_vcp, UART_DBG_NUM);
        if (moved == 0U) ca_os_sleep(1);
    }
}

static void bridge_cmd(const char *p)
{
    p = ca_skip_sp(p);
    if (ca_strncmp(p, "echo", 4) == 0) { bridge_echo(); return; }
    if (ca_strncmp(p, "conv", 4) == 0) { bridge_conv(); return; }
    if (ca_strncmp(p, "baud", 4) == 0) {
        const i32 b = ca_strtol(ca_skip_sp(p + 4), 10);
        if (b >= 1200 && b <= 4000000) s_br_baud = (u32)b;
        CLI_PRINTF_N("bridge baud: %u", s_br_baud);
        return;
    }
    CLI_PUT_N("bridge    : echo | conv | baud <n>   (UART " SLAVE_PINS ")");
    CLI_PRINTF_N("bridge    : baud %u, echo needs jumper TX<->RX, conv lasts until RESET",
                 s_br_baud);
}

/* ============================================================================
 * CLI
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("hello | status | show | wire | bridge echo|conv|baud <n>");
        return false;
    }
    if (ca_strncmp(p, "bridge", 6) == 0) {
        bridge_cmd(p + 6);
        return true;
    }
    if (ca_strncmp(p, "wire", 4) == 0) {
        /* Continuity test of the two crossed jumpers, on plain GPIO: no UART
         * involved, so a FAIL here is the wire and not the baud rate, the AF
         * or the pin map. Reprogramming the pins as plain GPIO below takes them
         * away from the UART (MODER stops being "alternate function"), so no
         * deinit call is needed - and there is none in the ca_uart_* API.
         * The pair is CROSSED: master TX reaches slave RX, and back. */
        test_wire();
        /* The pins are plain GPIO now, not UART AF: reboot rather than trying
         * to put both peripherals back by hand. The delay leaves the result up. */
        CLI_PUT_N("  rebooting in 2.5 s to restore the UARTs...");
        ca_os_sleep(2500);
        ca_os_reset();
        return true;
    }
    if (ca_strncmp(p, "hello", 5) == 0) {
        uart_send(UART_MASTER, CMD_HELLO, NULL, 0);
        g_tx_hello++;
        CLI_PUT_N("HELLO sent");
        return true;
    }
    if (ca_strncmp(p, "status", 6) == 0) {
        uart_send(UART_MASTER, CMD_STATUS, NULL, 0);
        g_tx_status++;
        CLI_PUT_N("STATUS sent");
        return true;
    }
    if (ca_strncmp(p, "show", 4) == 0) {
        CLI_PUT_N("--- Example 21 multi-instance UART ---");
        CLI_PUT_N("  wiring: " WIRE_1);
        CLI_PUT_N("          " WIRE_2);
        CLI_PRINTF_N("  uart init : master=%s  slave=%s",
                     g_master_up ? "OK" : "FAILED", g_slave_up ? "OK" : "FAILED");
        CLI_PUT_N("  [MASTER  " MASTER_PINS "]");
        CLI_PRINTF_N("    TX HELLO      : %u", g_tx_hello);
        CLI_PRINTF_N("    TX STATUS     : %u", g_tx_status);
        CLI_PUT_N("  [MASTER  RX  <-- slave]");
        CLI_PRINTF_N("    RX HELLO resp : %u  (chk_err=%u)", g_rx_hello,  g_rx_m.err);
        CLI_PRINTF_N("    RX STATUS resp: %u", g_rx_status);
        CLI_PUT_N("  [SLAVE   " SLAVE_PINS "  <-- master]");
        CLI_PRINTF_N("    Cmds handled  : %u  (chk_err=%u)", g_slave_cmds, g_rx_s.err);
        CLI_PRINTF_N("    Slave ticks   : %u  (x10ms)", g_slave_ticks);
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
    /* Master + slave UART are brought up from inside task_master/task_slave,
     * not here: ca_uart_init() called pre-scheduler in ca_os_configure() was
     * the actual defect on U575 — see
     * project_ex21_u575_led_init_kills_stdout.md. */

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

    /* Onboard LEDs: LD1 master RX, LD2 slave RX, LD3 checksum error.
     * Ports and pins come from this board's objmap.h. */
    EVAL_LED( ca_led_init(); )
}

void ca_os_main(void)
{
    g_master_task = ca_os_job_suspend("master", CA_TH_LEVEL2, task_master, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    g_slave_task  = ca_os_job_suspend("slave",  CA_TH_LEVEL2, task_slave,  NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_job(        "work",   CA_TH_LEVEL2, task_work,   NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    EVAL_CLI( ca_cli_register(&s_user_cli_node); )

    ca_os_run(true);
}


#else  /* EX21_OK == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N.
 * Two ways to get here: ENABLE_UART is off, or this board has no mapping for
 * the two peer UARTs the example wires together. */
PRIVATE_STRING(m21a, "=== Example 21 ===");
#if !ENABLE_UART
PRIVATE_STRING(m21b, "ENABLE_UART == 0 (skipping UART example)");
#else
PRIVATE_STRING(m21b, "example not supported on this MCU (no UART pair mapped)");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m21a);
    PUT_N(&priv_m21b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EX21_OK */
