// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 78 -- caOS Modbus TCP client over USB-NCM
 *
 * NCM variant of example_43 (same service on ETH). The application logic is
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically
 *   2. ARP service: server MAC resolution
 *   3. Modbus TCP client (port 502, unit_id 1):
 *        - FC03: reads 4 holding registers (addr 0..3)
 *        - FC06: writes a single register (addr 4)
 *        - FC16: writes 2 registers (addr 5..6)
 *        - FC01: reads 8 coils (addr 0..7)
 *        - cycle every MODBUS_POLL_PERIOD_MS
 *        - reconnects automatically if TCP drops
 *   4. ICMP echo reply (ping from the board)
 *   5. Diagnostics via CLI: "modbus stat" | "tcp list" | "net"
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side -- the DHCP server AND the Modbus server both run on the host:
 *   sudo python3 examples/example_78/modbus_server_ncm.py
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_TCP_SERVICE 1 | EN_MODBUS_SERVICE 1 |
 *                           EN_ARP_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#include "modbus_service.h"

/* The LED pins are NOT here: ca_led_* is used and the real board map lives
 * in objmap.h (CA_LED_TABLE), with LD1/LD2/LD3 at indices 1/2/3 on every
 * part. They used to be wired by MCU_ID and the F446 was lumped into the F407
 * branch, carrying the STM32F4VE pins (PA7/PA6), which are not LEDs on this
 * board - same defect found in ex77. */
#define EX78_LED_OK     1U   /* LD1 - transaction OK */
#define EX78_LED_HB_A   3U   /* LD3 - heartbeat A */
#define EX78_LED_HB_B   2U   /* LD2 - heartbeat B (opposite) */

/* ── Configuration for MCU family ─────────────────────────────────────── */

#if MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549) — UM2861 §6.4: LD1 green PC7 (SB21 ON, SB23 OFF),
 * LD2 blue PB7, LD3 red PG2. */
#define USB_NUM      0U

#elif MCU_ID == 753
/* NUCLEO-H753ZI (MB1364) — UM2407 §6.5: LD1 green PB0 (SB39 ON, SB47 OFF),
 * LD2 yellow PE1, LD3 red PB14. Lit with I/O HIGH.
 * NB PB7 is the blue LD2 of the U575, NOT of this board. */
#define USB_NUM      1U    /* OTG_FS CN14 without ULPI — change to 0 if ULPI is available */

#elif MCU_ID == 407 || MCU_ID == 446 || MCU_ID == 767
#define USB_NUM      0U

#elif MCU_ID == 563
/* NUCLEO-H563ZI (MB1404) — UM3115 §6.4: LD1 green PB0 (SB43 ON, SB51 OFF),
 * LD2 yellow PF4, LD3 red PG4. */
#define USB_NUM      0U          /* USB_DRD_FS — single instance H563 */

#else
/* example not supported by hardware */
#undef EN_USB
#define EN_USB 0
#endif


#if EN_NET && EN_USB && EN_USB_NCM && EN_ARP_SERVICE && \
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_MODBUS_SERVICE

/* ============================================================================
 * Constants
 * ============================================================================ */
#define MODBUS_SERVER_PORT    502U
#define MODBUS_UNIT_ID        1U

#define MODBUS_POLL_PERIOD_MS   3000U   /* poll cycle every 3 s */
#define MODBUS_RECONNECT_MS     5000U   /* wait between reconnection attempts */

/* Demo register addresses */
#define REG_SENSOR_BASE   0U    /* FC03: read 4 registers from addr 0 */
#define REG_SETPOINT      4U    /* FC06: write setpoint to addr 4 */
#define REG_CONFIG_BASE   5U    /* FC16: write 2 registers from addr 5 */
#define COIL_STATUS_BASE  0U    /* FC01: read 8 coils from addr 0 */

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x78U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile ca_ipv4_t s_gw         = { .addr = 0U };
static volatile bool   s_dhcp_bound = false;

static ca_semaphore_t s_dhcp_sem;

static volatile u32 s_tx_ok  = 0U;
static volatile u32 s_tx_err = 0U;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xB8U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * DHCP UDP handler (port 68)
 * ============================================================================ */
static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b   = ca_net_buf_ptr(buf_idx);
    u8           *frm = b->data + b->data_off;
    u16           l4  = b->meta.rx.l4_off;
    const ca_udp_frame_t *udp = (const ca_udp_frame_t *)(frm + l4);
    ca_dhcp_service_rx_handler(udp, (u16)(b->data_len - l4));
    ca_net_buf_free(buf_idx);
}

/* ============================================================================
 * DHCP callbacks
 * ============================================================================ */
static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    (void)mask; (void)dns;
    s_ip         = ip;
    s_gw         = gw;          /* the PC: gateway and Modbus server in one */
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  GW=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3],
        gw.v[0], gw.v[1], gw.v[2], gw.v[3]);

    ca_arp_send_garp();

    /* Unblocks task_modbus. The trywait before the post is not cosmetic: the
     * semaphore has max_count 1 and ca_sem_post() trips CA_SAFEGUARD on
     * overflow, so a renew arriving before task_modbus has consumed the token
     * would halt the system. Draining first makes the pair idempotent. */
    ca_sem_trywait(&s_dhcp_sem);
    ca_sem_post(&s_dhcp_sem);
}

static void on_dhcp_expire(void)
{
    s_dhcp_bound = false;
    s_ip.addr    = 0U;
    ca_stdout_put_n("DHCP lease EXPIRED");
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },   /* 0.0.0.0 — updated after DHCP */
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x78U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX78: Modbus TCP client over USB-NCM ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("Modbus  port %u  unit_id %u",
        (u32)MODBUS_SERVER_PORT, (u32)MODBUS_UNIT_ID);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex43) */
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_printf_n("[USB] init FAILED"); return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_printf_n("[USB] add NCM FAILED - check EN_USB_NCM"); return 1U;
    }
    ca_usb_dev_mng_start();

    /* Attach NCM to the net core as push iface */
    if (!ca_net_init(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_printf_n("[NET] ca_net_init FAILED"); return 1U;
    }

    /* Init stack with IP=0, MAC known */
    ca_net_proto_init(&net_cfg);

    /* Register port 68 to receive OFFER/ACK. Modbus needs no bind: it is a
     * client, it opens its own TCP connection to the server. */
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);

    /* The DORA must not start before the host has brought the interface up.
     *
     * This gate has no equivalent on ETH and is NOT a formality: the NCM link
     * goes up only when the host selects alt setting 1 (usb_ncm.c), and until
     * then ca_usb_ncm_send_frame() drops every frame on the floor and returns
     * false. A DISCOVER sent too early is not delayed, it is LOST -- and the
     * DHCP client would spend its whole retry budget (DHCP_DISCOVER_RETRY_N x
     * DHCP_OFFER_TIMEOUT_MS) talking into a closed pipe.
     *
     * But the wait CANNOT sit before the loop below: enumeration needs this
     * task to be already servicing frames, so blocking here would hold off the
     * very event being waited for (that mistake stalls enumeration outright --
     * the host reports "device not accepting address"). The gate therefore
     * lives INSIDE the loop, and the DHCP task is released the first time the
     * link is seen up.
     *
     * The Modbus client needs no gate of its own: it waits on s_dhcp_sem,
     * posted from on_dhcp_bound, and a lease cannot exist while the link is
     * down. */
    ca_stdout_put_n("waiting for the host to bring the interface up...");

    bool dhcp_released = false;
    while (1) {
        if (!dhcp_released && ca_usb_ncm_is_up()) {
            dhcp_released = true;
            ca_stdout_put_n("USB NCM UP");
            /* NB ca_dhcp_service_bind() is deliberately NOT called: it blocks,
             * and would stop ca_net_process() from running -> the OFFER would
             * never be processed -> deadlock. The IP arrives through the
             * on_dhcp_bound callback from the DHCP task (LEVEL1). */
            ca_dhcp_service_net_ready();
            ca_stdout_put_n("DHCP: starting DORA...");
        }

        /* Timed, not ca_sem_wait(): before the link is up no frame ever
         * arrives, so a plain wait would park here and the gate above would
         * never be re-evaluated. The timeout makes this loop poll the link
         * while still sleeping between frames. */
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) {
            ca_net_process();
        }
    }
    return 0U;
}

/* ============================================================================
 * Modbus task
 * ============================================================================ */
static u32 task_modbus(void *arg)
{
    (void)arg;

    /* waits for DHCP bind */
    ca_sem_wait(&s_dhcp_sem);

    /* On a point-to-point USB link the gateway IS the PC, which is also where
     * the Modbus server runs -- same arrangement as ex43 on a LAN where the
     * test PC was the gateway too. */
    ca_ipv4_t server_ip = s_gw;
    ca_ipv4_t local_ip  = s_ip;

    ca_stdout_printf_n("MODBUS server %u.%u.%u.%u:%u",
        server_ip.v[0], server_ip.v[1],
        server_ip.v[2], server_ip.v[3],
        (u32)MODBUS_SERVER_PORT);

    ca_modbus_service_init(server_ip, MODBUS_SERVER_PORT,
                           local_ip, s_mac, MODBUS_UNIT_ID);

    ca_stdout_put_n("MODBUS: starting TCP connection...");
    ca_modbus_service_net_ready();

    u32 last_poll      = 0U;
    u32 last_reconnect = ca_os_tick_ms();   /* avoids immediate double init */
    u32 setpoint       = 100U;   /* demo value to write */

    while (1) {
        u32 now = ca_os_tick_ms();

        if (!ca_modbus_is_connected()) {
            if ((now - last_reconnect) >= MODBUS_RECONNECT_MS) {
                last_reconnect = now;
                ca_stdout_put_n("MODBUS: reconnect attempt...");
                local_ip  = s_ip;
                server_ip = s_gw;
                ca_modbus_service_init(server_ip, MODBUS_SERVER_PORT,
                                       local_ip, s_mac, MODBUS_UNIT_ID);
                ca_modbus_service_net_ready();
            }
            ca_os_sleep(500U);
            continue;
        }

        if ((now - last_poll) >= MODBUS_POLL_PERIOD_MS) {
            last_poll = now;
            bool ok   = true;

            /* FC03 — read 4 holding registers */
            u16 regs[4] = { 0U };
            if (ca_modbus_read_holding_regs(REG_SENSOR_BASE, 4U, regs)) {
                ca_stdout_printf_n(
                    "FC03 regs[0..3]= %u %u %u %u",
                    (u32)regs[0], (u32)regs[1],
                    (u32)regs[2], (u32)regs[3]);
            } else {
                ca_stdout_put_n("FC03 ERR");
                ok = false;
            }

            /* FC06 — write setpoint */
            if (ok) {
                if (ca_modbus_write_single_reg(REG_SETPOINT, (u16)setpoint)) {
                    ca_stdout_printf_n("FC06 setpoint=%u OK", setpoint);
                    setpoint = (setpoint >= 1000U) ? 100U : setpoint + 100U;
                } else {
                    ca_stdout_put_n("FC06 ERR");
                    ok = false;
                }
            }

            /* FC16 — write 2 config registers */
            if (ok) {
                u16 cfg_vals[2] = { 0x0001U, 0x0002U };
                if (ca_modbus_write_multiple_regs(REG_CONFIG_BASE,
                                                  cfg_vals, 2U)) {
                    ca_stdout_put_n("FC16 config OK");
                } else {
                    ca_stdout_put_n("FC16 ERR");
                    ok = false;
                }
            }

            /* FC01 — read 8 coils */
            if (ok) {
                u8 coils[1] = { 0U };
                if (ca_modbus_read_coils(COIL_STATUS_BASE, 8U, coils)) {
                    ca_stdout_printf_n("FC01 coils=0x%02X", (u32)coils[0]);
                } else {
                    ca_stdout_put_n("FC01 ERR");
                    ok = false;
                }
            }

            if (ok) {
                s_tx_ok++;
                ca_led_on(EX78_LED_OK);
                ca_os_sleep(50U);
                ca_led_off(EX78_LED_OK);
            } else {
                s_tx_err++;
            }
            ca_stdout_printf_n("cycle done  ok=%u err=%u",
                (u32)s_tx_ok, (u32)s_tx_err);
        }

        ca_os_sleep(200U);
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex43
 * ============================================================================ */
static u32 task_ncm_mon(void *arg)
{
    (void)arg;
    bool prev = false;
    bool hb   = false;

    while (1) {
        bool up = ca_usb_ncm_is_up();
        if (up != prev) {
            ca_stdout_printf_n("USB NCM LINK %s", up ? "UP" : "DOWN");
            /* Coming back up with a lease already taken: ask for a renew so the
             * address is re-validated instead of being trusted until T1. */
            if (up && s_dhcp_bound) {
                ca_stdout_put_n("link back UP: requesting lease renew");
                ca_dhcp_service_renew();
            }
            prev = up;
        }

        /* Heartbeat in exclusive alternation: the two pins are written together with
         * opposite values, never with two independent toggles -- two toggles
         * could drift apart (a fault, a task restart) and end up both on or
         * both off. Writing hb and !hb, the invariant "one on, one off" holds
         * by construction at every round. */
        hb = !hb;
        if (hb) { ca_led_on(EX78_LED_HB_A);  ca_led_off(EX78_LED_HB_B); }
        else    { ca_led_off(EX78_LED_HB_A); ca_led_on(EX78_LED_HB_B);  }
        ca_os_sleep(500);
    }
    return 0U;
}

/* Pool/stats diagnostics on request via CLI "net", "modbus stat" and
 * "tcp list": the "eth" commands live inside #if EN_ETH, not built on NCM. */

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
        ca_cli_register(&s_dbg_cli);
    )

    /* All LEDs are configured HERE, before any task runs: LED_OK is driven from
     * task_modbus and the heartbeat pair from task_ncm_mon, so leaving the
     * setup inside either task would let the other write to a pin that is not
     * an output yet. */
    ca_led_init();
    ca_led_off(EX78_LED_OK);
    /* initial state already alternating: A off, B on */
    ca_led_off(EX78_LED_HB_A);
    ca_led_on(EX78_LED_HB_B);

    /* ca_net_init() is NOT called here (unlike ex43): the NCM iface only exists
     * after the USB device is up, so both live in task_net.
     * ca_modbus_service_init() is not here either -- it needs the server
     * address, which comes from the DHCP lease, so it lives in task_modbus. */

    {
        ca_ipv4_t zero = { .addr = 0U };
        ca_arp_service_init(s_mac, zero);
    }
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);

    ca_sem_init(&s_dhcp_sem, "DHCPBND", 0U, 1U);
}

void ca_os_main(void)
{
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_tcp_service_start();

    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("modbus",    CA_TH_LEVEL3, task_modbus,    NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m78a, "=== Example 78 ===");
PRIVATE_STRING(m78b, "EN_NET / EN_USB / EN_USB_NCM / EN_MODBUS_SERVICE / EN_TCP_SERVICE == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m78a);
    PUT_N(&priv_m78b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_MODBUS_SERVICE && ... */
