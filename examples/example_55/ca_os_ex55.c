// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 55 -- caOS Net Stack on USB-NCM: ARP/ICMP via net_proto
 *
 * Variant of example_33 (net stack on ETH) but over USB Ethernet (CDC-NCM).
 * Uses the ca_net_iface_t abstraction: ca_ncm_iface_get() registers the
 * ca_usb_ncm service as a PUSH network interface in the net core. The IP stack (ARP +
 * ICMP echo) is identical to ex33 -> the device responds to REAL ping via usb0.
 *
 *   ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM);
 *   ca_usb_dev_mng_add(&s_ncm.header);
 *   ca_usb_dev_mng_start();
 *   ca_net_init(ca_ncm_iface_get(&s_ncm));   // iface push
 *   ca_net_proto_init(&net_cfg);             // ARP/ICMP
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0).
 *
 * PC Procedure (X = interface usb0 / enxNN):
 *   sudo ip addr add 192.168.10.1/24 dev usb0
 *   sudo ip link set usb0 up
 *   ping -c 5 192.168.10.55          # ICMP echo response from device
 *   arping -c 3 -I usb0 192.168.10.55
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1
 */

#include "caos.h"
#include "net_proto.h"

/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
#define USB_NUM      0U
#define EP_BULK_MPS  64U

#elif MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */
#define EP_BULK_MPS  64U   /* 64 for OTG_FS, 512 for OTG_HS ULPI */

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U
#define EP_BULK_MPS  64U

#elif MCU_ID == 563
#define USB_NUM      0U          /* USB_DRD_FS — single instance H563 */
#define EP_BULK_MPS  64U

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#endif


#if EN_NET && EN_USB && EN_USB_NCM


/* ── Network identity ────────────────────────────────────────────────────── */
static const ca_mac_t  s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x55U } };
static const ca_ipv4_t s_ip  = { .v = { 192U, 168U, 10U, 55U } };

static const ca_net_cfg_t net_cfg = {
    .ip   = { .v = { 192U, 168U, 10U, 55U } },
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x55U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xA5U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ── Net task ─────────────────────────────────────────────────────────────── */
static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_printf_n("\n\n=== EX55: caOS net stack su USB-NCM ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("IP   %u.%u.%u.%u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3]);

    /* USB NCM */
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) { ca_stdout_printf_n("[USB] init FAILED"); return 1U; }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_printf_n("[USB] add NCM FAILED - check EN_USB_NCM"); return 1U;
    }
    ca_usb_dev_mng_start();

    /* Attach NCM to the net core as push iface + protocols */
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_printf_n("[NET] ca_net_init FAILED"); return 1U;
    }
    ca_net_proto_init(&net_cfg);   /* also registers the 'ping'/'ip' CLI (EN_CLI_NET) */

    ca_stdout_printf_n("Net task ready - bring up usb0 then ping %u.%u.%u.%u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3]);
    ca_stdout_printf_n("CLI: 'ping <ip> [count=N] [time=T]' to ping a host from here");

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0;
}

/* ── Stats task (every 5 s) ────────────────────────────────────────────────── */
static u32 task_stats(void *arg)
{
    (void)arg;
    while (1) {
        ca_os_sleep(5000);
        ca_net_stats_t st;
        ca_net_stats(&st);
        ca_stdout_printf_n(
            "STATS link=%s pool_free=%u/%u rx=%u drop_pool=%u no_handler=%u tx=%u tx_fail=%u proc=%u",
            ca_usb_ncm_is_up() ? "up" : "down",
            (u32)ca_net_buf_free_count(), (u32)NET_BUF_COUNT,
            st.rx_frames, st.rx_dropped_pool, st.rx_no_handler,
            st.tx_frames, st.tx_failed, st.process_calls);
    }
    return 0;
}

/* ── Heartbeat ────────────────────────────────────────────────────────────── */
static u32 task_heartbeat(void *arg)
{
    (void)arg;
    ca_led_init();
    while (1) {
        ca_led_toggle(1U);
        ca_os_sleep(500);
    }
    return 0;
}

/* ── Diagnostic CLI ──────────────────────────────────────────────────────── */
#if ENABLE_CLI
static bool dbg_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "usbdbg", 6) != 0) return false;
    EVAL_USB_DBG(ca_usb_dbg_print(USB_NUM);)
    return true;
}
static ca_cli_node_t s_dbg_cli = { dbg_cli_handler, NULL };
#endif

/* ── caOS hooks ───────────────────────────────────────────────────────────── */
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
        ca_cli_register(&s_dbg_cli);
    )
}

void ca_os_main(void)
{
    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("net_stats", CA_TH_LEVEL4, task_stats,     NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* EN_NET && EN_USB && EN_USB_NCM not enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m55a, "=== Example 55 ===");
PRIVATE_STRING(m55b, "EN_NET / EN_USB / EN_USB_NCM disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m55a);
    PUT_N(&priv_m55b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM */
