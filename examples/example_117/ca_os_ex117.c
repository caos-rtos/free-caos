// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 117 -- HIGH SPEED: USB composite CDC + MSC + NCM (3 classes on a single device)
 *
 * The high-speed twin of example 56 (EN_USB_HS=1, EN_USB_FS=0): same code,
 * NUCLEO-H7S3L8 only (OTG_HS + integrated HS PHY, USB-C CN2), bulk MPS from the
 * negotiated speed (cfg mps 0: 512 HS, 64 FS), USB PID = FS twin + 0x0100.
 * See doc/manual/S03_117_00_example_117.md for what differs.
 *
 * Demonstrates the ca_usb_dev_mng stack with THREE class services registered together:
 *   ca_usb_dev_mng_init(CA_USB_SPEED_HS, USB_NUM);
 *   ca_usb_dev_mng_add(&s_cdc.header);     // virtual serial port (/dev/ttyACMx)
 *   ca_usb_dev_mng_add(&s_msc.header);     // removable disk (128 KB ramdisk)
 *   ca_usb_dev_mng_add(&s_ncm.header);     // USB Ethernet (usb0) with caOS net stack
 *   ca_usb_dev_mng_start();
 *
 * The management builds a composite config descriptor (IAD for CDC) and
 * sums the PID contributions of the 3 classes. EP allocation (os_config.h):
 *   EP1 NCM data | EP2 MSC data | EP3 CDC data | EP4 CDC notif | EP5 NCM notif
 * = 5 data EPs: exactly saturates the U575 OTG_FS (max 5). The _Static_assert
 * D16 in usb_mgmt.h guarantees at compile-time the absence of collisions.
 *
 * The three application functions coexist:
 *   - CDC: serial echo (byte loopback) + banner on DTR
 *   - MSC: ramdisk on heap, host creates/mounts FAT
 *   - NCM: caOS net core (ARP + ICMP) -> the device responds to real ping
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1). F407 does NOT fit: OTG_FS = 5 EP
 * but the 3-class composite wants 5 data EPs and the F407 has an identical
 * internal OTG_FS (5) -> it fits; use USB_NUM=0. (H7 has margin up to 6.)
 *
 * Test from PC:
 *   lsusb -v -d ca01:                       # IAD CDC + Mass Storage + CDC-NCM
 *   sudo screen /dev/ttyACM0 115200         # serial echo
 *   sudo ./examples/example_53/test_fat.sh  # FAT on ramdisk
 *   sudo ip addr add 192.168.10.1/24 dev usb0 && sudo ip link set usb0 up
 *   ping -c 5 192.168.10.117                 # ICMP response from device
 *
 * REQUIREMENTS os_config.h:
 *   EN_USB 1 | EN_USB_CDC 1 | EN_USB_MSC 1 | EN_USB_NCM 1 | EN_NET 1
 */

#include "caos.h"
#include "net_proto.h"

#if MCU_ID == 773 && EN_USB_HS
#define USB_NUM      0U          /* OTG_HS, integrated HS PHY (Type-C CN2) */
#define EP_BULK_MPS  512U        /* buffer size: the HS packet (cfg mps is 0 = per speed) */

#else
/* example not supported by hardware */
#define EX56_NO_HW_ROOM  1
#undef EN_USB
#define EN_USB 0
#endif




/* F407 OTG_FS has only 3 data EPs (EP1..3): the 3-class composite (5 EPs) does not
 * fit → on F407 this example is NCM+MSC (2 classes, EP1/2 NCM + EP3 MSC),
 * without CDC. On U575/H753 it remains CDC+MSC+NCM (5 EPs). */
#if MCU_ID == 407 || MCU_ID == 439
#define EX56_HAS_CDC  0   /* F407 OTG_FS: 3 data EPs → only NCM+MSC */
#else
#define EX56_HAS_CDC  1   /* H563/H753/U575: ≥5 data EPs → CDC+MSC+NCM */
#endif

#if EN_USB && EN_USB_MSC && EN_USB_NCM && EN_NET && (!EX56_HAS_CDC || EN_USB_CDC)



#if EX56_HAS_CDC
/* ════════════════════════════════════════════════════════════════════════════
 *  CDC -- serial echo (byte loopback)   [excluded on F407: only NCM+MSC]
 * ════════════════════════════════════════════════════════════════════════════ */

#define CDC_MPS       0U    /* 0 = per speed: 512 HS, 64 FS */
#define NOTIFY_RX     (1U << 0)

/* Handle, not thid: a thid is a list position and moves when a task below it ends. */
static ca_task_handle_t s_echo_task = CA_TASK_HANDLE_NONE;
static volatile u8  s_dtr;              /* last known DTR state */

/* PULL mode: the ISR only wakes the echo task, which drains with
 * ca_usb_cdc_read(). Unread packets stay in the driver queue and the endpoint
 * NAKs the host: a flood is slowed down, never dropped (the old ISR ring was). */
static void cdc_rx_ready(void)
{
    ca_notify_send_handle_isr(s_echo_task, NOTIFY_RX, CA_NOTIFY_ACTION_SET_BITS);
}

PRIVATE_STRING(hello, "caOS composite CDC+MSC+NCM -- echo on\r\n");

static void cdc_line(const ca_usb_cdc_line_t *line, u8 control)
{
    (void)line;
    s_dtr = (u8)(control & 0x01U);             /* bit0 = DTR (terminal open) */
    if (s_dtr) {
        ca_usb_cdc_write((p_str)&priv_hello);
    }
}

static const ca_usb_cdc_cfg_t s_cdc = {
    .header  = { CA_USB_TYPE_CDC, &ca_usb_cdc_ops },
    .mps     = CDC_MPS,
    .on_rx_ready = cdc_rx_ready,
    .on_line = cdc_line,
};

static u32 task_echo(void *arg)
{
    (void)arg;
    static string msg;
    while (1) {
        (void)ca_notify_wait(true, 2000U);
        /* Drain everything that is pending: one notify can stand for several
         * packets, and a packet can be bigger than a string (512 B at HS). */
        u16 n;
        while ((n = ca_usb_cdc_read((u8 *)msg.data, (u16)sizeof(msg.data))) > 0U) {
            msg.size = n;
            while (!ca_usb_cdc_write(&msg))
                ca_os_yield();
        }
    }
    return 0U;
}
#endif /* EX56_HAS_CDC */

/* ════════════════════════════════════════════════════════════════════════════
 *  MSC -- ramdisk on heap
 * ════════════════════════════════════════════════════════════════════════════ */

#define BLK_SIZE   512U
#if MCU_ID == 407 || MCU_ID == 439
#define BLK_COUNT  128U                  /* F407: 64 KB (RAM 128K, minimum FAT12) */
#else
#define BLK_COUNT  256U                  /* 256 * 512 = 128 KB */
#endif
#define DISK_BYTES (BLK_SIZE * BLK_COUNT)

static u8 *s_disk;                        /* allocated in ca_os_configure */

static bool ramdisk_read(u32 lba, u8 *buf, u32 count)
{
    if (s_disk == NULL || (lba + count) > BLK_COUNT) return false;
    ca_memcpy(buf, &s_disk[lba * BLK_SIZE], count * BLK_SIZE);
    return true;
}

static bool ramdisk_write(u32 lba, const u8 *buf, u32 count)
{
    if (s_disk == NULL || (lba + count) > BLK_COUNT) return false;
    ca_memcpy(&s_disk[lba * BLK_SIZE], buf, count * BLK_SIZE);
    return true;
}

static ca_usb_msc_cfg_t s_msc = {
    .header      = { CA_USB_TYPE_MSC, &ca_usb_msc_ops },
    .mps         = 0U,
    .block_size  = BLK_SIZE,
    .block_count = BLK_COUNT,
    .read        = ramdisk_read,
    .write       = ramdisk_write,
};

/* ════════════════════════════════════════════════════════════════════════════
 *  NCM -- USB Ethernet + caOS net stack (ARP/ICMP)
 * ════════════════════════════════════════════════════════════════════════════ */

static const ca_ipv4_t s_ip  = { .v = { 192U, 168U, 10U, 117U } };

static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 117U } },
    .mac  = CA_EXAMPLE_MAC(117),
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 0U,
    .mac    = CA_EXAMPLE_MAC_HOST(117),  /* host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ════════════════════════════════════════════════════════════════════════════
 *  Management: registers the 3 classes, starts USB stack + net core
 * ════════════════════════════════════════════════════════════════════════════ */

static u32 task_usb(void *arg)
{
    (void)arg;
    ca_os_sleep(300U);

#if EX56_HAS_CDC
    ca_stdout_printf_n("\n\n=== EX56: USB composito CDC+MSC+NCM ===");
#else
    ca_stdout_printf_n("\n\n=== EX56: USB composito NCM+MSC (F407, no CDC) ===");
#endif

    if (s_disk == NULL) {
        ca_stdout_printf_n("[MSC] ERROR: ramdisk %lu KB not allocated (heap exhausted by NET?)",
                           (u32)(DISK_BYTES / 1024U));
    } else {
        ca_stdout_printf_n("[MSC] ramdisk %lu KB @ %p OK", (u32)(DISK_BYTES / 1024U), (void*)s_disk);
    }
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_HS, USB_NUM)) {
        ca_stdout_put_n("[USB] init FAILED");
        return 1U;
    }
#if EX56_HAS_CDC
    if (!ca_usb_dev_mng_add(&s_cdc.header)) { ca_stdout_put_n("[USB] add CDC FAILED"); return 1U; }
#endif
    if (!ca_usb_dev_mng_add(&s_msc.header)) { ca_stdout_put_n("[USB] add MSC FAILED"); return 1U; }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) { ca_stdout_put_n("[USB] add NCM FAILED"); return 1U; }
    ca_usb_dev_mng_start();

    /* Hooks NCM to the net core as iface push + ARP/ICMP protocols. */
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_put_n("[NET] ca_net_init FAILED");
        return 1U;
    }
    ca_net_proto_init(&net_cfg);   /* also registers the 'ping'/'ip' CLI (EN_CLI_NET) */

    ca_stdout_printf_n("[USB] composite up: %sramdisk %lu KB + usb0",
                       EX56_HAS_CDC ? "/dev/ttyACMx + " : "", (u32)(DISK_BYTES / 1024U));
    ca_stdout_printf_n("[NET] ping %u.%u.%u.%u (bring up usb0 first)",
                       s_ip.v[0], s_ip.v[1], s_ip.v[2], s_ip.v[3]);

    /* Loop: drains the net core (push from NCM) + reports CDC DTR changes. */
#if EX56_HAS_CDC
    u8 dtr_prev = 0U;
#endif
    while (1) {
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) {
            ca_net_process();
        }
#if EX56_HAS_CDC
        u8 dtr = s_dtr;
        if (dtr != dtr_prev) {
            ca_stdout_put_n(dtr ? "[USB] CDC terminal opened (DTR on)"
                                : "[USB] CDC terminal closed (DTR off)");
            dtr_prev = dtr;
        }
#endif
    }
    return 0U;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  Stats + heartbeat
 * ════════════════════════════════════════════════════════════════════════════ */

static u32 task_stats(void *arg)
{
    (void)arg;
    while (1) {
        ca_os_sleep(5000U);
        ca_net_stats_t st;
        ca_net_stats(&st);
        ca_stdout_printf_n(
            "STATS ncm=%s cdc_dtr=%u pool=%u/%u rx=%u tx=%u tx_fail=%u",
            ca_usb_ncm_is_up() ? "up" : "down",
#if EX56_HAS_CDC
            s_dtr,
#else
            0U,
#endif
            (u32)ca_net_buf_free_count(), (u32)NET_BUF_COUNT,
            st.rx_frames, st.tx_frames, st.tx_failed);
    }
    return 0U;
}

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
    )
    s_disk = (u8 *)ca_memory_allocate(DISK_BYTES);
}

void ca_os_main(void)
{
#if EX56_HAS_CDC
    s_echo_task = ca_os_job("echo", CA_TH_LEVEL5, task_echo, NULL,
                               STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
#endif
    ca_os_job("usb",   CA_TH_LEVEL1, task_usb,   NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("stats", CA_TH_LEVEL4, task_stats, NULL,
              STACK_SMALL,  CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("hb",    CA_TH_LEVEL4, task_hb,    NULL,
              STACK_SMALL,  CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* EN_USB && EN_USB_MSC && EN_USB_NCM && EN_NET && (!EX56_HAS_CDC || EN_USB_CDC) not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m117a, "=== Example 117 ===");
#ifdef EX56_NO_HW_ROOM
/* Naming the flags here would be a lie: the config has EN_USB, EN_USB_CDC,
 * EN_USB_MSC and EN_USB_NCM all at 1, and the #else above turned EN_USB off by
 * hand. Two reasons, and the endpoint one is not about memory at all. */
PRIVATE_STRING(m117b, "USB high speed: NUCLEO-H7S3L8 only (EN_USB_HS)");
#else
PRIVATE_STRING(m117b, "USB high speed: NUCLEO-H7S3L8 only (EN_USB_HS)");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m117a);
    PUT_N(&priv_m117b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_USB && EN_USB_CDC && EN_USB_MSC && EN_USB_NCM && EN_NET */
