// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 50 -- USB bare-metal: vendor-class bulk loopback
 *
 * Supported hardware:
 *   NUCLEO-H753ZI   — OTG_HS ULPI (USB_NUM=0, MPS=512)
 *                     or OTG_FS CN14  (USB_NUM=1, MPS=64)
 *   NUCLEO-U575ZI-Q — internal OTG_FS     (USB_NUM=0, MPS=64)
 *   NUCLEO-F407ZG   — internal OTG_FS     (USB_NUM=0, MPS=64)
 *
 * Features:
 *   1. USB enumeration as vendor-class device
 *   2. EP0 control: GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION
 *   3. EP1..EP6 bulk IN/OUT: loopback (OUT → IN), one task per EP
 *      (composite device: each EP is independent, per-EP driver queue)
 *   4. CLI: usb status | halt | resume | ep
 *
 * Test from PC:
 *   lsusb                         # verify VID=0xCA01 PID=0x0001
 *   sudo python3 test_usb_loopback.py --ep 6
 *
 * REQUIREMENTS os_config.h:
 *   EN_USB 1
 */

#include "caos.h"

/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
#define USB_NUM      0U
#define EP_BULK_MPS  64U

#elif MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */
#define EP_BULK_MPS  64U   /* 64 for OTG_FS, 512 for OTG_HS ULPI */

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U          /* OTG_FS, same layout on F407 and F446 */
#define EP_BULK_MPS  64U

#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS — single instance H563 */
#define EP_BULK_MPS  64U

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#endif


#if EN_USB


#define EP_BULK_IN   0x81U
#define EP_BULK_OUT  0x01U
#define EP_MPS_LO    ((u8)( EP_BULK_MPS        & 0xFFU))
#define EP_MPS_HI    ((u8)((EP_BULK_MPS >> 8U) & 0xFFU))

/* Composite test device: NUM_DATA_EP independent bulk EPs (EP1..EPn),
 * one loopback task per EP. Must be <= driver's USB_NQEP and
 * compatible with OTG_FS FIFO budget. If changed, also update the
 * EP_DESC list in s_cfg_desc and FW_DEPTH in test_usb_loopback.py.
 * Cap H7=6, U5=5: config descriptor 18+N*14 ≤ 127B (EP0 XFRSIZ limit,
 * no data-stage chunking). U575 has USB_EP_MAX=6 → only 5 data EPs. */
#if MCU_ID == 575
#define NUM_DATA_EP  5U
#elif MCU_ID == 407 || MCU_ID == 446
#define NUM_DATA_EP  3U    /* F407/F446 OTG_FS: USB_EP_MAX=4 → EP0 + 3 data EPs (EP1..3).
                            * Without the 446 here the #else below claimed 6 EPs in the
                            * config descriptor while the hardware has 3: the host opened
                            * all six and EP4..6 answered with I/O errors. */
#elif MCU_ID == 563
#define NUM_DATA_EP  7U    /* H563 USB_DRD_FS: USB_EP_MAX=8 → EP0 + 7 data EPs (EP1..7) */
#elif MCU_ID == 767
#define NUM_DATA_EP  5U    /* F767 OTG_FS: USB_EP_MAX=6 → EP0 + 5 data EPs (EP1..5).
                            * RM0410 Table 331. Without this the #else claimed 6. */
#else
#define NUM_DATA_EP  6U
#endif

/* Endpoint descriptor (7B): bulk, MPS=EP_BULK_MPS, bInterval=0 */
#define EP_DESC(addr)  7, 0x05, (addr), 0x02, EP_MPS_LO, EP_MPS_HI, 0

/* Standard USB request codes */
#define USB_REQ_GET_DESCRIPTOR    0x06U
#define USB_REQ_SET_ADDRESS       0x05U
#define USB_REQ_SET_CONFIGURATION 0x09U
#define USB_DESC_TYPE_DEVICE      0x01U
#define USB_DESC_TYPE_CONFIG      0x02U
#define USB_DESC_TYPE_STRING      0x03U

/* ── USB Descriptors ──────────────────────────────────────────────────────── */

static const u8 s_dev_desc[18] = {
    18,         /* bLength */
    0x01,       /* bDescriptorType: Device */
    0x00, 0x02, /* bcdUSB: 2.00 */
    0xFF,       /* bDeviceClass:    Vendor Specific */
    0xFF,       /* bDeviceSubClass: Vendor Specific */
    0xFF,       /* bDeviceProtocol: Vendor Specific */
    64,         /* bMaxPacketSize0: 64 B */
    (u8)(USB_VID & 0xFFU), (u8)(USB_VID >> 8), /* idVendor: os_config.h (0xCA01 in development) */
    0x01, 0x00, /* idProduct: 0x0001 */
    0x00, 0x01, /* bcdDevice: 1.00 */
    0x01,       /* iManufacturer: index 1 */
    0x02,       /* iProduct:      index 2 */
    0x00,       /* iSerialNumber: none */
    0x01        /* bNumConfigurations: 1 */
};

/* wTotalLength = config(9) + interface(9) + NUM_DATA_EP*2 endpoint(7) */
#define CFG_TOTAL_LEN  (9U + 9U + NUM_DATA_EP * 2U * 7U)

static const u8 s_cfg_desc[CFG_TOTAL_LEN] = {
    9, 0x02, (u8)(CFG_TOTAL_LEN & 0xFFU), (u8)(CFG_TOTAL_LEN >> 8U),
             1, 1, 0, 0xC0, 50,                              /* Config */
    9, 0x04, 0, 0, (u8)(NUM_DATA_EP * 2U), 0xFF, 0xFF, 0xFF, 0, /* Interface: vendor */
    /* NUM_DATA_EP bulk IN/OUT pairs (EP1..EPn). Per-EP guard: U5 max 5,
     * F407 max 3 (USB_EP_MAX=4), H7 up to 6. The number of rows MUST match
     * NUM_DATA_EP, otherwise the array (sized CFG_TOTAL_LEN) is truncated. */
    EP_DESC(0x81), EP_DESC(0x01),
    EP_DESC(0x82), EP_DESC(0x02),
    EP_DESC(0x83), EP_DESC(0x03),
#if NUM_DATA_EP >= 4
    EP_DESC(0x84), EP_DESC(0x04),
#endif
#if NUM_DATA_EP >= 5
    EP_DESC(0x85), EP_DESC(0x05),
#endif
#if NUM_DATA_EP >= 6
    EP_DESC(0x86), EP_DESC(0x06),
#endif
#if NUM_DATA_EP >= 7
    EP_DESC(0x87), EP_DESC(0x07),
#endif
};

static const u8 s_str0[]  = { 4,  0x03, 0x09, 0x04 };
static const u8 s_str1[]  = { 10, 0x03, 'c',0,'a',0,'O',0,'S',0 };
static const u8 s_str2[]  = { 32, 0x03,
    'U',0,'S',0,'B',0,' ',0,'V',0,'e',0,'n',0,'d',0,
    'o',0,'r',0,' ',0,'T',0,'e',0,'s',0,'t',0 };

/* ── USB Status (task + CLI access) ──────────────────────────────────────── */

typedef enum {
    USB_ST_IDLE       = 0,
    USB_ST_ATTACHED,       /* after the first bus reset */
    USB_ST_ADDRESSED,      /* after SET_ADDRESS */
    USB_ST_CONFIGURED,     /* after SET_CONFIGURATION */
    USB_ST_HALTED,         /* after CLI "usb halt" */
} usb_dev_st_t;

static volatile usb_dev_st_t  s_dev_state  = USB_ST_IDLE;
static volatile ca_usb_speed_t s_speed     = CA_USB_SPEED_FS;
static volatile u32  s_reset_cnt  = 0;
static volatile u32  s_setup_cnt  = 0;
static volatile u32  s_rx_cnt     = 0;
static volatile u32  s_tx_cnt     = 0;
static volatile u32  s_err_cnt    = 0;   /* unhandled SETUP requests */
static volatile u32  s_ep_user_closed_mask = 0U;   /* bit i = EPi closed via CLI */

/* Debug ring of SETUP requests that fall into STALL (max 16, wrap-around).
   Inspectable via GDB (s_dbg_stall_buf / s_dbg_stall_cnt) or via CLI 'usb errs'. */
#define DBG_STALL_DEPTH 16U
static volatile u8  s_dbg_stall_buf[DBG_STALL_DEPTH][8];
static volatile u32 s_dbg_stall_cnt = 0U;   /* total STALL issued on EP0 */

/* ── Shared state ISR ↔ task ──────────────────────────────────────────── */

/* Handle, not thid: a thid is a list position and moves when a task below it ends. */
static ca_task_handle_t s_ctrl_task = CA_TASK_HANDLE_NONE;  /* control task (EP0/setup/reset) */
static ca_task_handle_t s_loop_task[NUM_DATA_EP + 1U];       /* loopback task, idx 1..NUM_DATA_EP */
static volatile u8   s_setup_data[8];
static volatile bool s_setup_pending;
static volatile u8   s_pending_addr;

/* one loopback buffer per EP (ep_write copies before returning → reusable) */
static u8 s_loop_buf[NUM_DATA_EP + 1U][EP_BULK_MPS] __attribute__((aligned(4)));

static const char *const s_loop_names[8U] = {
    "", "lp1", "lp2", "lp3", "lp4", "lp5", "lp6", "lp7"
};

#define NOTIFY_SETUP    (1U << 0)
#define NOTIFY_BULK_RX  (1U << 1)
#define NOTIFY_STATUS   (1U << 2)   /* state change — log from task */

/* ── USB callbacks (called by ISR) ─────────────────────────────────────── */

static void on_reset(void)
{
    s_pending_addr  = 0;
    s_setup_pending = false;
    s_dev_state     = USB_ST_ATTACHED;
    s_reset_cnt++;
    ca_notify_send_handle_isr(s_ctrl_task, NOTIFY_STATUS, CA_NOTIFY_ACTION_SET_BITS);
}

static void on_mounted(ca_usb_speed_t speed)
{
    s_speed = speed;
}

static void on_setup(const u8 data[8])
{
    for (u8 i = 0U; i < 8U; i++) s_setup_data[i] = data[i];
    s_setup_pending = true;
    s_setup_cnt++;
    ca_notify_send_handle_isr(s_ctrl_task, NOTIFY_SETUP, CA_NOTIFY_ACTION_SET_BITS);
}

static void on_rx(u8 ep_addr, u16 len)
{
    (void)len;
    u8 ep = ep_addr & 0x7FU;
    if (ep >= 1U && ep <= NUM_DATA_EP) {
        s_rx_cnt++;
        /* wake up the loopback task dedicated to this EP */
        ca_notify_send_handle_isr(s_loop_task[ep], NOTIFY_BULK_RX, CA_NOTIFY_ACTION_SET_BITS);
    }
}

static volatile u32 s_ep0_tx_done_cnt = 0;
static volatile u8  s_last_applied_addr = 0;

static void on_tx_done(u8 ep_addr)
{
    if ((ep_addr & 0x7FU) == 0U) {
        s_ep0_tx_done_cnt++;
        if (s_pending_addr != 0U) {
            ca_usb_set_address(USB_NUM, s_pending_addr);
            s_last_applied_addr = s_pending_addr;
            s_pending_addr = 0U;
        }
    }
    /* EP bulk IN: TX completion is handled entirely by the driver
       (per-EP TX queue) → no action required here. */
}

/* ── SETUP handler (task context) ────────────────────────────────────────── */

static void handle_setup(void)
{
    u8 setup[8];
    for (u8 i = 0U; i < 8U; i++) setup[i] = s_setup_data[i];
    s_setup_pending = false;

    u8  bm  = setup[0];
    u8  req = setup[1];
    u16 val = (u16)setup[2] | ((u16)setup[3] << 8U);
    u16 wln = (u16)setup[6] | ((u16)setup[7] << 8U);

    if (bm == 0x80U && req == USB_REQ_GET_DESCRIPTOR) {
        u8 dtype = (u8)(val >> 8U);
        u8 didx  = (u8)(val & 0xFFU);
        const u8 *desc = NULL;
        u16       dlen = 0U;
        if      (dtype == USB_DESC_TYPE_DEVICE) { desc = s_dev_desc; dlen = (u16)sizeof(s_dev_desc); }
        else if (dtype == USB_DESC_TYPE_CONFIG)  { desc = s_cfg_desc; dlen = (u16)sizeof(s_cfg_desc); }
        else if (dtype == USB_DESC_TYPE_STRING) {
            if      (didx == 0U) { desc = s_str0; dlen = (u16)sizeof(s_str0); }
            else if (didx == 1U) { desc = s_str1; dlen = (u16)sizeof(s_str1); }
            else if (didx == 2U) { desc = s_str2; dlen = (u16)sizeof(s_str2); }
        }
        if (desc != NULL) {
            if (dlen > wln) dlen = wln;
            ca_usb_ep_write(USB_NUM, 0x80U, desc, dlen);
        } else {
            s_err_cnt++;
            ca_usb_ep_stall(USB_NUM, 0x80U);
        }

    } else if (bm == 0x00U && req == USB_REQ_SET_ADDRESS) {
        /* DWC2 quirk (ST HAL pattern): apply DCFG.DAD IMMEDIATELY, before
         * the ZLP status. The core internally handles the transition:
         * the ZLP status goes out with the old address, the next SETUP
         * arrives on the new one. Apply deferred (in on_tx_done) is race-prone:
         * XFRC ZLP triggers in a few μs and the host can send the next
         * SETUP before we apply DCFG. */
        u8 addr = (u8)(val & 0x7FU);
        ca_usb_set_address(USB_NUM, addr);
        s_last_applied_addr = addr;
        s_pending_addr = 0U;
        ca_usb_ep_write(USB_NUM, 0x80U, NULL, 0U);   /* ZLP IN status */
        s_dev_state = USB_ST_ADDRESSED;

    } else if (bm == 0x00U && req == USB_REQ_SET_CONFIGURATION) {
        for (u8 e = 1U; e <= NUM_DATA_EP; e++) {
            ca_usb_ep_open(USB_NUM, (u8)(e | 0x80U), CA_USB_EP_TYPE_BULK, EP_BULK_MPS); /* IN */
            ca_usb_ep_open(USB_NUM, e,               CA_USB_EP_TYPE_BULK, EP_BULK_MPS); /* OUT */
        }
        s_ep_user_closed_mask = 0U;
        ca_usb_ep_write(USB_NUM, 0x80U, NULL, 0U);
        s_dev_state = USB_ST_CONFIGURED;

    } else if ((bm == 0x02U || bm == 0x82U) &&
               (req == 0x01U /*CLEAR_FEATURE*/ || req == 0x03U /*SET_FEATURE*/) &&
               val == 0x0000U /*ENDPOINT_HALT*/) {
        /* CLEAR/SET_FEATURE(ENDPOINT_HALT): pyusb and the kernel emit them at
           config release; without a handler the device STALLs and the host
           reacts with a bus reset. */
        u8 ep_addr = (u8)(setup[4]);    /* wIndex low byte = ep_addr */
        if (req == 0x01U) ca_usb_ep_clear_stall(USB_NUM, ep_addr);
        else              ca_usb_ep_stall(USB_NUM, ep_addr);
        ca_usb_ep_write(USB_NUM, 0x80U, NULL, 0U);   /* ZLP IN status */

    } else if (bm == 0x82U && req == 0x00U /*GET_STATUS endpoint*/) {
        u8 ep_addr = (u8)(setup[4]);
        u8 status[2] = { ca_usb_ep_is_stalled(USB_NUM, ep_addr) ? 0x01U : 0x00U, 0x00U };
        u16 dlen = (wln < 2U) ? wln : 2U;
        ca_usb_ep_write(USB_NUM, 0x80U, status, dlen);

    } else if ((bm == 0x80U || bm == 0x81U) && req == 0x00U /*GET_STATUS dev/iface*/) {
        /* Device: bit0=Self-Powered, bit1=Remote-Wakeup → both 0 (bus-powered) */
        u8 status[2] = { 0x00U, 0x00U };
        u16 dlen = (wln < 2U) ? wln : 2U;
        ca_usb_ep_write(USB_NUM, 0x80U, status, dlen);

    } else {
        s_err_cnt++;
        u32 slot = s_dbg_stall_cnt & (DBG_STALL_DEPTH - 1U);
        for (u8 i = 0U; i < 8U; i++) s_dbg_stall_buf[slot][i] = setup[i];
        s_dbg_stall_cnt++;
        ca_usb_ep_stall(USB_NUM, 0x80U);
    }
}

/* ── Bulk loopback ────────────────────────────────────────────────────────── */

/* Drain the RX queue of the ep EP and re-emit every packet on the homologous IN.
   ep_write copies the buffer before returning → s_loop_buf[ep] reusable.
   Each EP has its own driver queue (per-EP) and its own task → no contention. */
static void loop_drain(u8 ep)
{
    u8  ep_out = ep;
    u8  ep_in  = (u8)(ep | 0x80U);
    u8 *buf    = s_loop_buf[ep];
    u16 len;
    while ((len = ca_usb_ep_read(USB_NUM, ep_out, buf, EP_BULK_MPS)) > 0U) {
        /* Do not drop: spin-yield until ep_write accepts (TX driver queue
           handles backpressure; ep_write returns false only when queue is full). */
        while (!ca_usb_ep_write(USB_NUM, ep_in, buf, len))
            ca_os_yield();
        s_tx_cnt++;
    }
}

/* ── State change log (task context) ─────────────────────────────────────── */

static void handle_status(void)
{
    static const char *const state_name[] = {
        "IDLE", "ATTACHED", "ADDRESSED", "CONFIGURED", "HALTED"
    };
    u32 st = (u32)s_dev_state;
    if (st > (u32)USB_ST_HALTED) st = 0U;
    const char *spd = (s_speed == CA_USB_SPEED_HS) ? "HS" : "FS";
    ca_stdout_printf_n("[USB] state=%-10s speed=%s resets=%lu",
                       state_name[st], spd, s_reset_cnt);
}

/* ── CLI ──────────────────────────────────────────────────────────────────── */

#if ENABLE_CLI

static bool usb_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("usb: status | halt | resume | ep");
        return false;
    }
    if (ca_strncmp(p, "usb", 3) != 0) return false;
    p = ca_skip_sp(p + 3);

    if (ca_strncmp(p, "status", 6) == 0) {
        static const char *const sname[] = {
            "IDLE", "ATTACHED", "ADDRESSED", "CONFIGURED", "HALTED"
        };
        u32 st = (u32)s_dev_state;
        if (st > (u32)USB_ST_HALTED) st = 0U;
        const char *spd = (s_speed == CA_USB_SPEED_HS) ? "HS" : "FS";
        CLI_PRINTF_N("[USB] state=%-10s speed=%s resets=%lu setup=%lu",
                     sname[st], spd, s_reset_cnt, s_setup_cnt);
        CLI_PRINTF_N("[USB] rx=%lu tx=%lu err=%lu",
                     s_rx_cnt, s_tx_cnt, s_err_cnt);
        return true;
    }

#if EN_USB_DBG
    if (ca_strncmp(p, "dbg reset", 9) == 0) {
        ca_usb_dbg_reset();
        CLI_PUT_N("[USB] dbg counters reset");
        return true;
    }
    if (ca_strncmp(p, "dbg", 3) == 0) {
        ca_usb_dbg_t d;
        ca_usb_dbg_stat(&d, USB_NUM);
        u32 isr_avg = d.isr_rx_cnt ? (d.isr_rx_sum / d.isr_rx_cnt) : 0U;
        CLI_PRINTF_N("[USB] ISR OUT XFRC: n=%lu  avg=%lu cyc (%lu ns)  max=%lu cyc (%lu ns)",
                     d.isr_rx_cnt, isr_avg, isr_avg * 25U / 12U,
                     d.isr_rx_max, d.isr_rx_max * 25U / 12U);
        u32 lat_avg = d.lat_cnt[1] ? (d.lat_sum[1] / d.lat_cnt[1]) : 0U;
        CLI_PRINTF_N("[USB] OUT XFRC->IN arm EP1: n=%lu  avg=%lu cyc (%lu us)  max=%lu cyc (%lu us)",
                     d.lat_cnt[1], lat_avg, lat_avg / 480U,
                     d.lat_max[1], d.lat_max[1] / 480U);
        u32 iat_avg = d.iat_cnt ? (d.iat_sum / d.iat_cnt) : 0U;
        u32 pps     = iat_avg ? (480000000U / iat_avg) : 0U;
        CLI_PRINTF_N("[USB] inter-arrival OUT: n=%lu  avg=%lu us  min=%lu us  max=%lu us  -> %lu pps host",
                     d.iat_cnt, iat_avg / 480U, d.iat_min / 480U, d.iat_max / 480U, pps);
        return true;
    }
    if (ca_strncmp(p, "eps", 3) == 0) {
        /* Per-EP dump for perennial NAK diagnosis (H563): CHEP register + ring.
           STAT: 0=DIS 1=STALL 2=NAK 3=VALID. Compare a reused EP (e.g. EP1)
           with a fresh one (e.g. EP3) after a failed test. */
        static const char *const stat_nm[4] = { "DIS", "STL", "NAK", "VLD" };
        ca_usb_dbg_t d;
        ca_usb_dbg_stat(&d, USB_NUM);
        for (u8 e = 1U; e <= NUM_DATA_EP; e++) {
            u32  c   = d.chep[e];
            u8   srx = (u8)((c >> 12) & 0x3U);
            u8   stx = (u8)((c >>  4) & 0x3U);
            u8   drx = (u8)((c >> 14) & 0x1U);
            u8   dtx = (u8)((c >>  6) & 0x1U);
            CLI_PRINTF_N("EP%u CHEP=%08lx RX=%s TX=%s Drx=%u Dtx=%u | rxq h%u/t%u txq h%u/t%u act=%u",
                         e, c, stat_nm[srx], stat_nm[stx], drx, dtx,
                         d.rxq_h[e], d.rxq_t[e], d.txq_h[e], d.txq_t[e], d.tx_act[e]);
            CLI_PRINTF_N("    oxfrc=%lu ixfrc=%lu rd=%lu wr=%lu wrfull=%lu",
                         d.oep_xfrc[e], d.iep_xfrc[e], d.ep_read[e], d.ep_write[e], d.wr_full[e]);
            CLI_PRINTF_N("    rxdrop=%lu rxcnt0=%lu rxlen0=%lu",
                         d.rx_drop[e], d.rx_cnt0[e], d.rx_len0[e]);
        }
        return true;
    }
#endif

    if (ca_strncmp(p, "errs", 4) == 0) {
        CLI_PRINTF_N("[USB] STALL total: %lu  (ring last %u)", s_dbg_stall_cnt, DBG_STALL_DEPTH);
        u32 n = (s_dbg_stall_cnt < DBG_STALL_DEPTH) ? s_dbg_stall_cnt : DBG_STALL_DEPTH;
        u32 start = (s_dbg_stall_cnt >= DBG_STALL_DEPTH)
                  ? (s_dbg_stall_cnt & (DBG_STALL_DEPTH - 1U)) : 0U;
        for (u32 i = 0U; i < n; i++) {
            u32 idx = (start + i) & (DBG_STALL_DEPTH - 1U);
            CLI_PRINTF_N("  #%lu bm=%02x req=%02x val=%02x%02x idx=%02x%02x len=%02x%02x",
                         i, s_dbg_stall_buf[idx][0], s_dbg_stall_buf[idx][1],
                         s_dbg_stall_buf[idx][3], s_dbg_stall_buf[idx][2],
                         s_dbg_stall_buf[idx][5], s_dbg_stall_buf[idx][4],
                         s_dbg_stall_buf[idx][7], s_dbg_stall_buf[idx][6]);
        }
        return true;
    }

    if (ca_strncmp(p, "halt", 4) == 0) {
        if (s_dev_state == USB_ST_HALTED) {
            CLI_PUT_N("[USB] already halted");
        } else {
            ca_usb_disconnect(USB_NUM);
            s_dev_state = USB_ST_HALTED;
            CLI_PUT_N("[USB] halted (soft disconnect)");
        }
        return true;
    }

    if (ca_strncmp(p, "resume", 6) == 0) {
        if (s_dev_state != USB_ST_HALTED) {
            CLI_PUT_N("[USB] not halted");
        } else {
            ca_usb_connect(USB_NUM);
            s_dev_state = USB_ST_IDLE;
            CLI_PUT_N("[USB] resume (host re-enumerate)");
        }
        return true;
    }

    if (ca_strncmp(p, "ep", 2) == 0 &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t' || p[2] == '\r' || p[2] == '\n')) {
        CLI_PUT_N("[USB] EP0  ctrl   64B  always open");
        if (s_dev_state == USB_ST_CONFIGURED) {
            for (u8 e = 1U; e <= NUM_DATA_EP; e++) {
                const char *state = (s_ep_user_closed_mask & (1U << e)) ? "closed" : "open";
                const char *stall = (ca_usb_ep_is_stalled(USB_NUM, (u8)(e | 0x80U)) ||
                                     ca_usb_ep_is_stalled(USB_NUM, e)) ? " [STALL]" : "";
                CLI_PRINTF_N("[USB] EP%u  bulk  %uB  %s%s",
                             e, EP_BULK_MPS, state, stall);
            }
        } else {
            CLI_PRINTF_N("[USB] EP1..EP%u  bulk   --   not open (not configured)",
                         NUM_DATA_EP);
        }
        return true;
    }

    /* usb open|close|stall|unstall <ep> — runtime EP API test (Patch 1) */
    {
        bool is_open    = (ca_strncmp(p, "open",    4) == 0);
        bool is_close   = (ca_strncmp(p, "close",   5) == 0);
        bool is_stall   = (ca_strncmp(p, "stall",   5) == 0);
        bool is_unstall = (ca_strncmp(p, "unstall", 7) == 0);
        if (is_open || is_close || is_stall || is_unstall) {
            if (s_dev_state != USB_ST_CONFIGURED) {
                CLI_PUT_N("[USB] device not configured");
                return true;
            }
            const char *q = ca_skip_sp(p + (is_open ? 4 : is_unstall ? 7 : 5));
            u16 t = 0U;
            i32 ep = ca_strtol_index(q, 10, &t);
            if (t == 0U || ep < 1 || ep > (i32)NUM_DATA_EP) {
                CLI_PRINTF_N("usb: ep must be 1..%u", NUM_DATA_EP);
                return true;
            }
            u8 e    = (u8)ep;
            u8 ein  = (u8)(e | 0x80U);
            u8 eout = e;
            if (is_close) {
                bool ok_in  = ca_usb_ep_close(USB_NUM, ein);
                bool ok_out = ca_usb_ep_close(USB_NUM, eout);
                if (ok_in || ok_out) s_ep_user_closed_mask |= (1U << e);
                CLI_PRINTF_N("[USB] close EP%u  IN=%s OUT=%s", e,
                             ok_in ? "ok" : "skip", ok_out ? "ok" : "skip");
            } else if (is_open) {
                bool ok_in  = ca_usb_ep_open(USB_NUM, ein,  CA_USB_EP_TYPE_BULK, EP_BULK_MPS);
                bool ok_out = ca_usb_ep_open(USB_NUM, eout, CA_USB_EP_TYPE_BULK, EP_BULK_MPS);
                if (ok_in && ok_out) s_ep_user_closed_mask &= ~(1U << e);
                CLI_PRINTF_N("[USB] open  EP%u  IN=%s OUT=%s", e,
                             ok_in ? "ok" : "ko", ok_out ? "ok" : "ko");
            } else if (is_stall) {
                ca_usb_ep_stall(USB_NUM, ein);
                ca_usb_ep_stall(USB_NUM, eout);
                CLI_PRINTF_N("[USB] stall EP%u  IN+OUT", e);
            } else { /* unstall */
                bool ok_in  = ca_usb_ep_clear_stall(USB_NUM, ein);
                bool ok_out = ca_usb_ep_clear_stall(USB_NUM, eout);
                CLI_PRINTF_N("[USB] unstall EP%u  IN=%s OUT=%s", e,
                             ok_in ? "ok" : "ko", ok_out ? "ok" : "ko");
            }
            return true;
        }
    }

    CLI_PUT_N("usb: status | errs | halt | resume | ep | open|close|stall|unstall <ep>");
    return true;
}

static ca_cli_node_t s_usb_cli = { usb_cli_handler, NULL };

#endif /* ENABLE_CLI */

/* ── USB Task ─────────────────────────────────────────────────────────────── */

static u32 task_usb(void *arg)
{
    (void)arg;

    static const ca_usb_cb_t cb = {
        .on_reset     = on_reset,
        .on_mounted   = on_mounted,
        .on_unmounted = NULL,
        .on_setup     = on_setup,
        .on_rx        = on_rx,
        .on_tx_done   = on_tx_done,
    };

    ca_stdout_put_n("[USB] init...");
    if (!ca_usb_init(CA_USB_SPEED_FS, USB_NUM, &cb)) {
        ca_stdout_put_n("[USB] init FAILED - check clock/GPIO");
        return 1U;
    }
    /* Direct driver (no mng_start): raise D+ pull-up here (SDIS=0). ca_usb_init
       leaves it down on purpose (fix early pull-up: the host must not enumerate before
       the callbacks/descriptors are ready — now they are). Without this, at reset the
       device does not appear until 'usb resume' is executed. */
    ca_usb_connect(USB_NUM);
    ca_stdout_put_n("[USB] ready - plug cable");

    while (1) {
        u32 ev = ca_notify_wait(true, 2000U);

        if (ev & NOTIFY_STATUS)  handle_status();
        if (ev & NOTIFY_SETUP)   handle_setup();
    }
    return 0U;
}

/* ── Loopback task (one per bulk EP) ───────────────────────────────────────
   p1 = EP number (1..NUM_DATA_EP). Waits for NOTIFY_BULK_RX on its mailbox
   (sent by on_rx for that EP) and drains the EP's RX queue into its IN. */
static u32 task_loop(void *arg)
{
    const u32 p1 = (u32)(uintptr_t)arg;
    u8 ep = (u8)p1;
    while (1) {
        (void)ca_notify_wait(true, 2000U);
        loop_drain(ep);
    }
    return 0U;
}

/* ── Heartbeat task ───────────────────────────────────────────────────────── */

static u32 task_hb(void *arg)
{
    (void)arg;
    ca_led_init();
    while (1) {
        ca_led_toggle(1U);
        ca_os_sleep(500U);
    }
    return 0U;
}

/* ── Stacks ───────────────────────────────────────────────────────────────── */



/* ── Entry point ──────────────────────────────────────────────────────────── */

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
        ca_cli_register(&s_usb_cli);
    )
}

void ca_os_main(void)
{
    /* Control task (EP0/setup/reset) — also performs ca_usb_init. */
    s_ctrl_task = ca_os_job("usb", CA_TH_LEVEL6, task_usb, NULL,
                               STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    if (s_ctrl_task != CA_TASK_HANDLE_NONE) {
        /* A loopback task for bulk EP (EP1..NUM_DATA_EP), equal priority
           → fair round-robin. The IDs are used by on_rx to route the notify. */
        for (u32 e = 1U; e <= NUM_DATA_EP; e++) {
            s_loop_task[e] = ca_os_job(s_loop_names[e], CA_TH_LEVEL5, task_loop, (void *)(uintptr_t)e,
                                          STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
        }
        ca_os_job("hb", CA_TH_LEVEL4, task_hb, NULL,
                  STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    }

    ca_os_run(true);
}

#else  /* EN_USB not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m50a, "=== Example 50 ===");
PRIVATE_STRING(m50b, "EN_USB disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m50a);
    PUT_N(&priv_m50b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB */
