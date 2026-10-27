// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 32 — ETH: link UP / link DOWN
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI) or NUCLEO-H563ZI (STM32H563ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *
 * Pin ETH RMII H753 (AF11, VERY_HIGH, NOPULL):
 *   PA1  — ETH_RMII_REF_CLK
 *   PA2  — ETH_MDIO
 *   PA7  — ETH_RMII_CRS_DV
 *   PC1  — ETH_MDC
 *   PC4  — ETH_RMII_RXD0
 *   PC5  — ETH_RMII_RXD1
 *   PG11 — ETH_RMII_TX_EN
 *   PB13 — ETH_RMII_TXD1
 *   PG13 — ETH_RMII_TXD0
 *
 * Pin ETH RMII H563 (AF11, VERY_HIGH, NOPULL) — UM3115 Table 16:
 *   same as H753 except TXD1 on PB15 (not PB13).
 *   PA1  — ETH_RMII_REF_CLK   (SB58 ON)
 *   PA2  — ETH_MDIO           (SB69 ON)
 *   PA7  — ETH_RMII_CRS_DV    (SB38 ON)
 *   PC1  — ETH_MDC            (SB62 ON)
 *   PC4  — ETH_RMII_RXD0      (SB42 ON)
 *   PC5  — ETH_RMII_RXD1      (SB36 ON)
 *   PG11 — ETH_RMII_TX_EN     (SB34 ON)
 *   PG13 — ETH_RMII_TXD0      (SB37 ON)
 *   PB15 — ETH_RMII_TXD1      (JP6 ON, SB10 OFF)
 *   NOTE: JP6 -> ON and SB10 -> OFF are required to bring TXD1 out on PB15;
 *    with SB10 ON the pin goes to the Zio as I2S_2_SD instead (UM3115 Table 17).
 *    eth_h5xx.c was HW-validated here on 2026-08-28: MDIO, link up/down, TX and
 *    RX all confirmed on the NUCLEO-H563ZI.
 *
 * LED:
 *   LED1 green   PB0  — on = link UP, off = link DOWN
 *   LED3 red     PB14 (H753) / PB1 free pin (H563, no 3rd onboard LED) — heartbeat 1 Hz
 *
 * Behavior:
 *   - ca_eth_link_poll() called every 500 ms by task_eth
 *   - on transition: USB CDC message + LED1
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH        1
 *   EVAL_USB_CDC  1  (or EVAL_STDOUT for UART debug)
 */

#include "caos.h"



#if EN_ETH && EN_NET

/* ============================================================================
 * HARDWARE
 * ============================================================================ */


#if MCU_ID == 563
#else
#endif

#define ETH_AF          11U          /* AF11 for all RMII pins on H753/H563 */

/* ============================================================================
 * RX dump
 * ============================================================================ */

#define DUMP_MAX      32U   /* max bytes hex-dumped per packet */
/* ============================================================================
 * Local MAC + IP
 * ============================================================================ */

static void on_link_change(bool up);   /* forward declaration */

/* Board IP address — update based on local subnet */
static const u8 board_ip[4] = { 192U, 168U, 10U, 20U };

static const ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x20U } },
    .link_cb  = on_link_change,
};


/* ============================================================================
 * Gratuitous ARP — announces MAC+IP on every LINK UP edge (on_link_change)
 *
 * Format (ARP request, 42 bytes + 18 bytes padding = 60 bytes minimum Ethernet):
 *   [0-5]   Dst  FF:FF:FF:FF:FF:FF  (broadcast)
 *   [6-11]  Src  board MAC
 *   [12-13] EtherType 0x0806 (ARP)
 *   [14-15] HTYPE 0x0001
 *   [16-17] PTYPE 0x0800
 *   [18]    HLEN 6
 *   [19]    PLEN 4
 *   [20-21] OPER 0x0001 (request)
 *   [22-27] SHA  board MAC
 *   [28-31] SPA  board IP
 *   [32-37] THA  00:00:00:00:00:00
 *   [38-41] TPA  board IP  (= SPA → gratuitous)
 *   [42-59] padding zeros
 * ============================================================================ */
static void send_gratuitous_arp(void)
{
    u16 idx = ca_net_buf_alloc();
    if (idx == NET_BUF_INVALID) { ca_stdout_printf_n("GARP: no buf"); return; }

    ca_net_buf_t *b = ca_net_buf_ptr(idx);
    u8  *arp = b->data + NET_BUF_HEADROOM;
    const u8 *mac = eth_cfg.mac.v;
    const u8 *ip  = board_ip;

    /* Ethernet header */
    for (u32 i = 0U; i < 6U; i++) arp[i]      = 0xFFU;
    for (u32 i = 0U; i < 6U; i++) arp[6U+i]   = mac[i];
    arp[12] = 0x08U; arp[13] = 0x06U;

    /* ARP payload */
    arp[14] = 0x00U; arp[15] = 0x01U;
    arp[16] = 0x08U; arp[17] = 0x00U;
    arp[18] = 6U;    arp[19] = 4U;
    arp[20] = 0x00U; arp[21] = 0x01U;

    for (u32 i = 0U; i < 6U; i++) arp[22U+i] = mac[i];
    for (u32 i = 0U; i < 4U; i++) arp[28U+i] = ip[i];
    for (u32 i = 0U; i < 6U; i++) arp[32U+i] = 0x00U;
    for (u32 i = 0U; i < 4U; i++) arp[38U+i] = ip[i];
    for (u32 i = 42U; i < 60U; i++) arp[i]   = 0x00U;

    b->data_off = NET_BUF_HEADROOM;
    b->data_len = 60U;
    i32 rc = ca_eth_send(idx);    /* driver owns buf, whatever rc says */
    if (rc == CA_ETH_SEND_OK)
        ca_stdout_printf_n("GARP %u.%u.%u.%u %02X:%02X:%02X:%02X:%02X:%02X OK",
            ip[0], ip[1], ip[2], ip[3],
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    else
        ca_stdout_printf_n("GARP FAIL %d%s", rc,
            rc == CA_ETH_SEND_ERR_LINK ? " (link down)" : " (tx ring)");
}

static void dump_packet(u8 *pkt, u32 len, u32 n)
{
    ca_stdout_printf_n("PKT#%u len=%u", n, len);
    if (len < 14U) return;
    ca_stdout_printf_n(
        "  D=%02X:%02X:%02X:%02X:%02X:%02X"
        "  S=%02X:%02X:%02X:%02X:%02X:%02X"
        "  t=0x%04X",
        pkt[0],pkt[1],pkt[2],pkt[3],pkt[4],pkt[5],
        pkt[6],pkt[7],pkt[8],pkt[9],pkt[10],pkt[11],
        ((u32)pkt[12] << 8) | pkt[13]);
    u32 d = len < DUMP_MAX ? len : DUMP_MAX;
    ca_hexdump(pkt, (u16)d);
}

/* buf_idx: pool buf with received frame; consumed here (sent or freed) */
static void swap_and_send(u16 buf_idx)
{
    ca_net_buf_t *b   = ca_net_buf_ptr(buf_idx);
    u8           *pkt = b->data + NET_BUF_HEADROOM;
    if (b->data_len < 14U) { ca_net_buf_free(buf_idx); return; }
    /* D = original source */
    for (u32 i = 0U; i < 6U; i++) pkt[i] = pkt[6U + i];
    /* S = board MAC */
    for (u32 i = 0U; i < 6U; i++) pkt[6U + i] = eth_cfg.mac.v[i];
    u32 len = b->data_len;            /* b is the driver's once sent */
    i32 rc  = ca_eth_send(buf_idx);   /* driver owns buf, whatever rc says */
    if (rc == CA_ETH_SEND_OK) ca_stdout_printf_n("TX len=%u OK", len);
    else                      ca_stdout_printf_n("TX len=%u FAIL %d", len, rc);
}

static u32 task_rx(void *arg)
{
    (void)arg;
    u32 count = 0U;
    while (1) {
        for (;;) {
            u16 new_idx = ca_net_buf_alloc();
            if (new_idx == NET_BUF_INVALID) break;   /* pool exhausted */

            u16 out_idx;
            if (!ca_eth_recv(&out_idx, new_idx)) {
                ca_net_buf_free(new_idx);             /* no frame */
                break;
            }
            ca_net_buf_t *b = ca_net_buf_ptr(out_idx);
            dump_packet(b->data + NET_BUF_HEADROOM, b->data_len, ++count);
            swap_and_send(out_idx);   /* consumes out_idx */
        }
        ca_os_sleep(10);
    }
    return 0;
}


/* ============================================================================
 * CALLBACK link state (called by task_eth via ca_eth_link_poll)
 * ============================================================================ */

static void on_link_change(bool up)
{
    if (up) {
        ca_led_on(1U);
        ca_stdout_printf_n("EX32 ETH: LINK UP");
        /* Announce on the rising edge, never blind after init: sent 500 ms
         * after ca_eth_init() it left ~0.9 s BEFORE the PC carrier came up
         * and was lost, while ca_eth_send() still said OK (H563 02/10, three
         * resets: GARP -0.9 s, LINK UP +0.12 s against the PC carrier). */
        send_gratuitous_arp();
    } else {
        ca_led_off(1U);
        ca_stdout_printf_n("EX32 ETH: LINK DOWN");
    }
}

/* ============================================================================
 * TASK: monitor link (poll every 500 ms)
 * ============================================================================ */

static u32 task_eth(void *arg)
{
    (void)arg;
    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    /* Allow time for tud_task() to run and for the terminal to be ready
     * before sending the first message. Without this sleep, ca_eth_init()
     * (busy-wait DMA + MDIO) prevents the USB CDC task from calling tud_task()
     * and the first tud_cdc_write() arrives too soon after connection. */
    ca_os_sleep(200);


    ca_stdout_printf_n("\n\nEX32: ETH INIT OK");
    ca_stdout_printf_n("PHY addr=%d RMII MAC %02X:%02X:%02X:%02X:%02X:%02X IP %u.%u.%u.%u",
        eth_cfg.phy_addr,
        eth_cfg.mac.v[0], eth_cfg.mac.v[1], eth_cfg.mac.v[2],
        eth_cfg.mac.v[3], eth_cfg.mac.v[4], eth_cfg.mac.v[5],
        board_ip[0], board_ip[1], board_ip[2], board_ip[3]);
    ca_os_sleep(500);
    ca_eth_link_poll();          /* GARP goes out from on_link_change(up) */
    /* Explicit yield: let stdout_thread send the init message
     * before ca_eth_link_poll() potentially queues "LINK UP" */
    ca_os_sleep(50);

    while (1) {
        ca_eth_link_poll();
        ca_os_sleep(500);
    }
    return 0;
}

/* ============================================================================
 * TASK: heartbeat
 * ============================================================================ */

static u32 task_heartbeat(void *arg)
{
    (void)arg;
    while (1) {
        ca_led_toggle(3U);
        ca_os_sleep(500);
    }
    return 0;
}

/* ============================================================================
 * GPIO init ETH RMII
 * ============================================================================ */

static void eth_gpio_init(void)
{
    /* PA1 REF_CLK, PA2 MDIO, PA7 CRS_DV */
    ca_gpio_configure(CA_GPIO_PORT_A, 1U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
    ca_gpio_configure(CA_GPIO_PORT_A, 2U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
    ca_gpio_configure(CA_GPIO_PORT_A, 7U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);

    /* PC1 MDC, PC4 RXD0, PC5 RXD1 */
    ca_gpio_configure(CA_GPIO_PORT_C, 1U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
    ca_gpio_configure(CA_GPIO_PORT_C, 4U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
    ca_gpio_configure(CA_GPIO_PORT_C, 5U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);

    /* PG11 TX_EN, TXD1 (PB13 H753 / PB15 H563), PG13 TXD0 */
    ca_gpio_configure(CA_GPIO_PORT_G, 11U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
#if MCU_ID == 563
    ca_gpio_configure(CA_GPIO_PORT_B, 15U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
#else
    ca_gpio_configure(CA_GPIO_PORT_B, 13U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
#endif
    ca_gpio_configure(CA_GPIO_PORT_G, 13U,
                      CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_VERY_HIGH,
                      CA_GPIO_NOPULL, ETH_AF);
}

/* ============================================================================
 * caOS hooks
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

    /* LED link state */
    
    ca_led_off(1U);

    /* LED heartbeat */
    ca_led_init();
    ca_led_off(3U);

    /* ETH GPIO — configured here: the driver does not manage pins */
    eth_gpio_init();
    ca_net_buf_pool_init();   /* pool required by ca_eth_send/recv zero-copy */
    if (!ca_eth_init(&eth_cfg)) {
        CA_SAFE_FAIL("ca_eth_init");
    }
}

void ca_os_main(void)
{
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth,       NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("eth_rx",    CA_TH_LEVEL3, task_rx,        NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("heartbeat", CA_TH_LEVEL3, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_run(true);
}

#else /* EN_ETH == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m32a, "=== Example 32: ETH link UP / link DOWN ===");
PRIVATE_STRING(m32b, "DISABLED - requires EN_ETH / EN_NET");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m32a);
    PUT_N(&priv_m32b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH */
