// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex63.c
 * @brief          : Example 63 — UART DMA with variable length protocol
 ******************************************************************************
 * @details
 * Demonstrates the UART driver DMA mode (EN_UART_DMA) in the most general
 * case: protocol with length NOT known a priori.
 *
 *   frame:  [SOF 0xCA][type][len_lo][len_hi]  payload[len]  [crc_lo][crc_hi]
 *
 * The header (4 bytes) arrives in normal mode: the on_rx callback (ISR context)
 * decodes it byte by byte. Once len is known, ca_uart_dma_recv() is called from
 * the ISR itself: RXNEIE off, the DMA receives payload+CRC without any
 * interrupt per byte. At TC (counter at zero) the driver restores the
 * header mode and wakes up the task in ca_uart_dma_wait(), which verifies the
 * CRC16-CCITT (on type+len+payload) and replies with an echo (type|0x80) via
 * ca_uart_dma_send() — TX is also DMA one-shot, task sleeping until TC.
 *
 * HOW TO RUN IT
 * -------------
 * The UART under test IS the console UART: EX63_UART equals UART_DBG_NUM on
 * every board (3 on H563/H753, 1 on U575/F407), because the test has to talk
 * to a PC and the ST-LINK VCP is the only link that needs no extra adapter.
 * So console and protocol would fight over the same lines: the host's binary
 * frames land in the CLI parser (which answers "huh!") and the CLI banner
 * corrupts the replies the host script expects.
 *
 * The console therefore moves to USB CDC and the debug UART is switched off.
 * Both are already set in os_config_ex_63.h; if you rebuild the config from
 * the model, restore them with:
 *
 *   python3 tools/caos_config.py --on EN_USB --on EN_USB_CDC \
 *       --on STDOUT_ON_USB_CDC --on STDIN_ON_USB_CDC \
 *       --off STDOUT_ON_UART --off STDIN_ON_UART \
 *       --off ENABLE_DBG_UART 63
 *
 * ENABLE_DBG_UART lives in objmap.h, which is included BEFORE the config, so
 * it needs an #undef before being redefined - caos_config.py emits it. Do NOT
 * edit objmap.h by hand: that would silence the console for every example.
 *
 * Then plug BOTH USB cables (ST-LINK and USB device) and use two ports:
 *
 *   host script -> the ST-LINK VCP        (/dev/ttyACM0, no banner on it now)
 *   console/CLI -> the CDC port, VID ca01 (/dev/ttyACM1)
 *
 *   python3 examples/example_63/host_test.py /dev/ttyACM0
 *
 * The script walks the payload lengths 1..1024 - including 255/256/257, which
 * exercise len_lo/len_hi and the DMA count crossing the high byte - then sends
 * a frame with a deliberately bad CRC (the device must NOT echo it) and checks
 * that the next frames still arrive, proving the resync back to header mode.
 *
 * CLI (on CDC): "u63 info"  — frame/CRC/resync statistics + DMA status
 *              "u63 abort" — cancels a stalled DMA reception
 *
 * NOTE H7xx: frame buffers must be in RAM D2 (no DTCM).
 ******************************************************************************
 */

#include "caos.h"

/* F4xx excluded. This example talks to a host PC over the ST-LINK VCP, so the
 * UART it opens has to be the one the virtual COM port is wired to. On the
 * NUCLEO-F446ZE that is USART3 (PD8/PD9, SB5/SB6 closed at the factory), but
 * USART3 is also UART_DBG_NUM there, so the CLI and the test would fight over
 * the same lines - the host script's frames end up in the CLI, which answers
 * "huh!" to the binary payload. USART1's PA9/PA10 are USB VBUS and USB ID on
 * this board (UM1974 Table 10), and its alternate map PB6/PB7 has PB7 as LD2.
 * Making it work would mean rerouting both the console and the test; not worth
 * it for a driver already covered on H563.
 *
 * Tested on MCU_ID, not on MCU: the family names are not numeric macros, so
 * "MCU == F4xx" compares 0 with 0 and holds everywhere. */
#if MCU_ID == 407 || MCU_ID == 429 || MCU_ID == 446
#define EX63_NO_FREE_UART  1
#endif

#if EN_UART_DMA && EN_DMA && !defined(EX63_NO_FREE_UART)
/* The UART under test is the console UART, so take it FROM objmap.h.
 *
 * The header above already says "EX63_UART equals UART_DBG_NUM": the whole
 * point is that the host script talks through the ST-LINK VCP, which is the
 * one link needing no extra adapter, and ENABLE_DBG_UART is turned off so the
 * CLI does not fight over it. Naming the number again here meant naming it
 * per board, and the list only ever covered the H563 - every other part fell
 * into "#else 1". On the H753 that is USART1 while its VCP is USART3
 * (objmap.h:114), so the example listened on a peripheral the host script was
 * never talking to: bytes = 0 after a full run, which reads as a broken DMA
 * path rather than as a wrong port.
 *
 * UART_DBG_NUM is the board's own answer to "which UART is the VCP", so use
 * it and let every board be right by construction. */
#define EX63_UART       UART_DBG_NUM
#define EX63_BAUD       CA_UART_BAUD_115200

#define FRM_SOF         0xCAU
#define FRM_MAX_PAYLOAD 1024U
#define FRM_CRC_SIZE    2U
#define FRM_HDR_SIZE    4U
#define EX63_STALL_MS   200U        /* no byte of a frame for this long: given up */

/* ---- frame buffer (H7: place in RAM D2) ---- */
static u8 s_rx_buf[FRM_MAX_PAYLOAD + FRM_CRC_SIZE];
static u8 s_tx_buf[FRM_HDR_SIZE + FRM_MAX_PAYLOAD + FRM_CRC_SIZE];

/* ---- header state (written only in ISR) ---- */
typedef enum { HDR_SOF = 0, HDR_TYPE, HDR_LEN_L, HDR_LEN_H } hdr_state_t;
static volatile hdr_state_t s_state;
static volatile u8  s_frm_type;
static volatile u16 s_frm_len;      /* latched at DMA arming */
static u16 s_len_tmp;

/* ---- statistics ---- */
static volatile u32 s_cnt_frames;   /* complete frames received (TC)     */
static volatile u32 s_cnt_bytes;    /* payload bytes received            */
static volatile u32 s_cnt_crc_err;  /* frames with incorrect CRC (no echo)    */
static volatile u32 s_cnt_len_err;  /* header with len out of range       */
static volatile u32 s_cnt_resync;   /* bytes discarded while hunting for SOF   */
static volatile u32 s_cnt_echo;     /* echoes transmitted                    */
static volatile u32 s_cnt_timeout;  /* frames given up half way (peer silent) */

/* ---- frame in progress (set in ISR at arming, cleared by the task) ---- */
static volatile bool s_rx_armed;
static u32           s_last_rem;    /* bytes missing at the previous check */

/* CRC16-CCITT (poly 0x1021, init 0xFFFF) on type+len+payload */
static u16 crc16_ccitt(u16 crc, const u8 *data, u32 len)
{
    while (len--) {
        crc ^= (u16)(*data++ << 8);
        for (u8 i = 0; i < 8U; i++)
            crc = (crc & 0x8000U) ? (u16)((crc << 1) ^ 0x1021U)
                                  : (u16)(crc << 1);
    }
    return crc;
}

/* Header phase — ISR context, one interrupt per byte only here */
static void ex63_on_rx(u32 rx_data)
{
    u8 b = (u8)rx_data;
    switch (s_state) {
        case HDR_SOF:
            if (b == FRM_SOF) s_state = HDR_TYPE;
            else              s_cnt_resync++;
            break;
        case HDR_TYPE:
            s_frm_type = b;
            s_state    = HDR_LEN_L;
            break;
        case HDR_LEN_L:
            s_len_tmp = b;
            s_state   = HDR_LEN_H;
            break;
        case HDR_LEN_H:
            s_len_tmp |= (u16)(b << 8);
            s_state    = HDR_SOF;
            if (s_len_tmp == 0U || s_len_tmp > FRM_MAX_PAYLOAD) {
                s_cnt_len_err++;
                break;
            }
            s_frm_len = s_len_tmp;
            /* from here payload+CRC in DMA: no interrupt until TC */
            s_last_rem = 0xFFFFFFFFU;
            s_rx_armed = ca_uart_dma_recv(EX63_UART, s_rx_buf, (u16)(s_frm_len + FRM_CRC_SIZE));
            break;
    }
}

/* Protocol task: sleeps until TC, verifies CRC and replies with echo */
static u32 task_frame(void *arg)
{
    (void)arg;
    ca_os_sleep(5000);  /* waits for CDC OK */
    EVAL_STDOUT(
        ca_stdout_put_n("\r\n=== Example 63: UART DMA variable-length protocol ===");
        ca_stdout_printf_n("UART%u %u 8N1 on the ST-LINK VCP (ENABLE_DBG_UART=0!)",
                           (u32)EX63_UART, (u32)EX63_BAUD);
        ca_stdout_put_n("host: python3 examples/example_63/host_test.py <port>");
        ca_stdout_put_n("CLI: 'u63 info' statistics, 'u63 abort' recovery");
    )
    while (1) {
        /* Never CA_WAIT_FOREVER: a peer that stops half way through a frame
         * would leave the DMA armed for good, and the NEXT frame would be
         * swallowed as the missing tail (CRC error, then everything after it
         * out of step). A frame that gets no byte for EX63_STALL_MS is given
         * up; one that is still moving, however slowly, is left alone. */
        if (!ca_uart_dma_wait(EX63_UART, EX63_STALL_MS)) {
            if (!s_rx_armed) continue;                    /* idle: no frame */
            const u32 rem = ca_uart_dma_remaining(EX63_UART);
            if (rem == 0U) continue;                      /* TC just in: next wait */
            if (rem != s_last_rem) { s_last_rem = rem; continue; }  /* moving */
            ca_uart_dma_abort(EX63_UART);   /* stops the stream, no TC follows */
            s_rx_armed = false;
            s_state    = HDR_SOF;
            s_cnt_timeout++;
            ca_stdout_printf_n("frame TIMEOUT: %u of %u bytes never came, back to header mode",
                               rem, (u32)s_frm_len + FRM_CRC_SIZE);
            continue;
        }
        s_rx_armed = false;

        u16 len  = s_frm_len;
        u8  type = s_frm_type;
        s_cnt_frames++;
        s_cnt_bytes += len;

        u8 hdr[3] = { type, (u8)(len & 0xFFU), (u8)(len >> 8) };
        u16 crc   = crc16_ccitt(0xFFFFU, hdr, sizeof(hdr));
        crc       = crc16_ccitt(crc, s_rx_buf, len);
        u16 rxcrc = (u16)(s_rx_buf[len] | (s_rx_buf[len + 1U] << 8));
        if (crc != rxcrc) {
            s_cnt_crc_err++;
            ca_stdout_printf_n("frame[%u] CRC_ERR: calc=0x%04X rx=0x%04X len=%u",
                             s_cnt_frames, crc, rxcrc, len);
            continue;                    /* corrupted frame: no echo */
        }else{
            ca_stdout_printf_n("frame[%u] OK: type=0x%02X len=%u",
                             s_cnt_frames, type, len);
        }

        u8 rtype  = (u8)(type | 0x80U);
        u8 rhdr[3] = { rtype, hdr[1], hdr[2] };
        u16 rcrc  = crc16_ccitt(0xFFFFU, rhdr, sizeof(rhdr));
        rcrc      = crc16_ccitt(rcrc, s_rx_buf, len);

        s_tx_buf[0] = FRM_SOF;
        s_tx_buf[1] = rtype;
        s_tx_buf[2] = hdr[1];
        s_tx_buf[3] = hdr[2];
        ca_memcpy(&s_tx_buf[FRM_HDR_SIZE], s_rx_buf, len);
        s_tx_buf[FRM_HDR_SIZE + len]      = (u8)(rcrc & 0xFFU);
        s_tx_buf[FRM_HDR_SIZE + len + 1U] = (u8)(rcrc >> 8);

        ca_uart_dma_send(EX63_UART, s_tx_buf,
                         (u16)(FRM_HDR_SIZE + len + FRM_CRC_SIZE));
        s_cnt_echo++;
    }
    return 0;
}

/* ---- CLI "u63" on CDC console ---- */
#if ENABLE_CLI
static bool ex63_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("u63: info | abort");
        return false;
    }
    if (ca_strncmp(p, "u63", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "info", 4) == 0) {
        CLI_PRINTF_N("frames  : %u  (echo %u)", s_cnt_frames, s_cnt_echo);
        CLI_PRINTF_N("bytes   : %u", s_cnt_bytes);
        CLI_PRINTF_N("crc_err : %u  len_err: %u  resync: %u",
                     s_cnt_crc_err, s_cnt_len_err, s_cnt_resync);
        CLI_PRINTF_N("timeout : %u", s_cnt_timeout);
        CLI_PRINTF_N("dma rem : %u", ca_uart_dma_remaining(EX63_UART));
        return true;
    }
    if (ca_strncmp(p, "abort", 5) == 0) {
        ca_uart_dma_abort(EX63_UART);
        s_rx_armed = false;
        s_state = HDR_SOF;
        CLI_PUT_N("u63: rx aborted, header mode restored");
        return true;
    }
    CLI_PUT_N("u63: info | abort");
    return false;
}

static ca_cli_node_t s_ex63_cli_node = { ex63_cli_handler, NULL };
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
    if (!ca_uart_init_dma(EX63_UART, EX63_BAUD, ex63_on_rx, 0U))
        CA_SAFEGUARD(0, "ex63: init uart dma failed");

    EVAL_CLI( ca_cli_register(&s_ex63_cli_node); )
}

void ca_os_main(void)
{
    ca_os_job("frame", CA_TH_LEVEL4, task_frame, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* EN_UART_DMA == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m63a, "=== Example 63 ===");
#ifdef EX63_NO_FREE_UART
PRIVATE_STRING(m63b, "F4xx: no UART free of the console - example not supported");
#else
PRIVATE_STRING(m63b, "EN_UART_DMA / EN_DMA disabled");
#endif
/* The bench's marker, spelled out rather than taken from
 * CA_EXAMPLE_UNSUPPORTED(): that macro goes through PUT_N, which this
 * example's config routes to the USB CDC - see ca_os_configure below. The
 * text must stay byte-identical to CA_EXAMPLE_UNSUPPORTED_MSG, which is why
 * it is built from that macro instead of being retyped. */
PRIVATE_STRING(ca_unsup_63, CA_EXAMPLE_UNSUPPORTED_MSG);

/* ca_uart_dbg_put cannot be used here: it compiles to `return 0` unless
 * STDOUT_ON_UART is set, and this example's config sets STDOUT_ON_USB_CDC
 * instead. Write to the debug UART directly. */
static u16 unsup_put(p_str message)
{
    return ca_uart_put(UART_DBG_NUM, message);
}
void ca_os_configure(void)
{
    /* Say it on the DEBUG UART, not through PUT_N.
     *
     * This example's config puts stdout on the USB CDC (STDOUT_ON_USB_CDC=1),
     * because when it runs it needs the console UART for its own DMA
     * protocol. But PUT_N follows that setting in this branch too, and here
     * the example is NOT running: on a board where it is unsupported the CDC
     * may never enumerate at all, and then the one line that says so goes
     * nowhere. The bench, listening on the ST-LINK VCP, sees silence and
     * records five failed phases instead of a `disabled` - which is what
     * happened on F446 (2026-09-07), a part this file has excluded on purpose
     * since EX63_NO_FREE_UART was introduced.
     *
     * The marker has to reach whoever is watching, so it goes out on the
     * board's own debug UART - the link that exists on every part and needs
     * no enumeration. */
    ca_uart_init(UART_DBG_NUM, CA_UART_BAUD_115200, NULL, 0);
    ca_put_n(unsup_put, &priv_ca_unsup_63);
    ca_put_n(unsup_put, &priv_m63a);
    ca_put_n(unsup_put, &priv_m63b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_UART_DMA && EN_DMA */
