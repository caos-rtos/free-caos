// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 77 -- caOS MQTT client over USB-NCM
 *               (publish status, receive LED commands)
 *
 * NCM variant of example_42 (same service on ETH). The application logic is
 * unchanged: what differs is only the interface handed to ca_net_init(), thanks
 * to the ca_net_iface_t abstraction.
 *
 * Functionality:
 *   1. DHCP client: obtains IP automatically
 *   2. ARP service: broker MAC resolution
 *   3. MQTT client (v3.1.1, QoS 0):
 *        - publishes to "caos/status" every MQTT_PUB_PERIOD_MS
 *          payload: {"seq":<n>,"up":<s>,"ip":"a.b.c.d"}
 *        - subscribes to "caos/led" → "on"/"off" → LED
 *        - keepalive ping every MQTT_PING_PERIOD_MS (< MQTT_KEEPALIVE_SEC)
 *        - automatically reconnects after disconnection
 *   4. ICMP echo reply (ping from the board)
 *   5. Diagnostics via CLI: "mqtt stat" | "tcp list" | "net"
 *
 * Hardware: U575 (USB_NUM=0), H753 (USB_NUM=1), F407 (USB_NUM=0), H563 (USB_NUM=0).
 *
 * PC side -- the DHCP server AND the MQTT broker both run on the host:
 *   sudo python3 examples/example_77/mqtt_server_ncm.py
 *
 * REQUIREMENTS os_config.h: EN_NET 1 | EN_USB 1 | EN_USB_NCM 1 |
 *                           EN_NET_DHCP 1 | EN_DHCP_SERVICE 1 |
 *                           EN_TCP_SERVICE 1 | EN_MQTT_SERVICE 1 |
 *                           EN_ARP_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"
#include "tcp_service.h"
#include "mqtt_service.h"

/* ── Configuration for MCU family ─────────────────────────────────────── */

/* Three LEDs, three distinct roles. The pins are NOT here: ca_led_* is used
 * and the real board map lives in objmap.h (CA_LED_TABLE), with LD1/LD2/LD3 at
 * indices 1/2/3 on every part. The pins used to be wired in this file by
 * MCU_ID, and the F446 was lumped into the F407 branch: it carried the
 * STM32F4VE pins (PA7/PA6), which are not LEDs on the NUCLEO-F446ZE. MQTT
 * switched some random GPIO and "MQTT LED -> ON" lit nothing.
 *
 *   LD1 -> driven by MQTT, topic "caos/led" (on/off)
 *   LD2 + LD3 -> heartbeat in EXCLUSIVE ALTERNATION: when one is on the other
 *                is off, and they swap at every beat.
 *
 * The alternation is driven by a single source (task_ncm_mon), which always
 * writes both with opposite values: so the two LEDs can never end up in the
 * same state, and no other context touches them. */
#define EX77_LED_MQTT   1U   /* LD1 */
#define EX77_LED_HB_A   3U   /* LD3 */
#define EX77_LED_HB_B   2U   /* LD2 */
#if MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549) — UM2861 §6.4: LD1 green PC7 (SB21 ON, SB23 OFF),
 * LD2 blue PB7, LD3 red PG2. */
#define USB_NUM      0U

#elif MCU_ID == 753
/* NUCLEO-H753ZI (MB1364) — UM2407 §6.5: LD1 green PB0 (SB39 ON, SB47 OFF),
 * LD2 yellow PE1, LD3 red PB14. Lit with I/O HIGH.
 * NB PB7 is the blue LD2 of the NUCLEO-U575ZI-Q, NOT of this board. */
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
    EN_DHCP_SERVICE && EN_NET_DHCP && EN_TCP_SERVICE && EN_MQTT_SERVICE

/* ============================================================================
 * Constants
 * ============================================================================ */
#define MQTT_BROKER_PORT    1883U
#define MQTT_CLIENT_ID      "caos-ncm"
#define MQTT_TOPIC_STATUS   "caos/status"
#define MQTT_TOPIC_LED      "caos/led"

#define MQTT_PUB_PERIOD_MS   5000U   /* publish status every 5 s */
#define MQTT_PING_PERIOD_MS  30000U  /* keepalive ping every 30 s (< MQTT_KEEPALIVE_SEC=60) */
#define MQTT_RECONNECT_MS    8000U   /* delay between reconnection attempts */

/* ============================================================================
 * Network — Fixed MAC, IP assigned by DHCP
 * ============================================================================ */
static const ca_mac_t s_mac = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x77U } };

static volatile ca_ipv4_t s_ip         = { .addr = 0U };
static volatile ca_ipv4_t s_gw         = { .addr = 0U };  /* gateway = broker IP */
static volatile bool   s_dhcp_bound = false;
static volatile u32    s_mqtt_conn  = 0U;   /* successful MQTT connection counter */

static ca_semaphore_t s_dhcp_sem;

/* ── USB NCM service (host-side MAC) ─────────────────────────────────────── */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
    .mac    = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0xB7U },  /* Host-side MAC */
    /* .on_rx set by ca_ncm_iface_get() */
};

/* ============================================================================
 * MQTT callbacks
 * ============================================================================ */
static void on_mqtt_connected(void)
{
    s_mqtt_conn++;
    ca_stdout_printf_n("MQTT CONNECTED  (#%u)", (u32)s_mqtt_conn);

    /* subscribe to LED command topic */
    ca_mqtt_subscribe(MQTT_TOPIC_LED);
    ca_stdout_put_n("MQTT subscribed -> " MQTT_TOPIC_LED);
}

static void on_mqtt_message(const char *topic, u16 topic_len,
                            const char *payload, u16 payload_len)
{
    /* use explicit lengths: topic and payload are NOT null-terminated */
    if (topic_len == (u16)(sizeof(MQTT_TOPIC_LED) - 1U) &&
        ca_memcmp(topic, MQTT_TOPIC_LED, topic_len) == 0)
    {
        if (payload_len >= 2U &&
            payload[0] == 'o' && payload[1] == 'n')
        {
            ca_led_on(EX77_LED_MQTT);
            ca_stdout_put_n("MQTT LED -> ON");
        } else {
            ca_led_off(EX77_LED_MQTT);
            ca_stdout_put_n("MQTT LED -> OFF");
        }
    }
}

static void on_mqtt_disconnected(void)
{
    ca_stdout_put_n("MQTT DISCONNECTED - reconnecting...");
}

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
    s_gw         = gw;          /* the PC: gateway and MQTT broker in one */
    s_dhcp_bound = true;

    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);

    ca_stdout_printf_n("\nDHCP BOUND  IP=%u.%u.%u.%u  GW=%u.%u.%u.%u",
        ip.v[0], ip.v[1], ip.v[2], ip.v[3],
        gw.v[0], gw.v[1], gw.v[2], gw.v[3]);

    ca_arp_send_garp();

    /* Unblocks task_mqtt. The trywait before the post is not cosmetic: the
     * semaphore has max_count 1 and ca_sem_post() trips CA_SAFEGUARD on
     * overflow, so a renew arriving before task_mqtt has consumed the token
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
    .mac  = { .v = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x00U, 0x77U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static u32 task_net(void *arg)
{
    (void)arg;
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX77: MQTT client over USB-NCM ===");
    ca_stdout_printf_n("MAC     %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("Topics  pub '%s'  sub '%s'",
        MQTT_TOPIC_STATUS, MQTT_TOPIC_LED);

    /* USB NCM bring-up (replaces eth_gpio_init() of ex42) */
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

    /* Register port 68 to receive OFFER/ACK. MQTT needs no bind: it is a
     * client, it opens its own TCP connection to the broker. */
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
     * The MQTT client needs no gate of its own: it waits on s_dhcp_sem, posted
     * from on_dhcp_bound, and a lease cannot exist while the link is down. */
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
 * MQTT task
 * ============================================================================ */
static u32 task_mqtt(void *arg)
{
    (void)arg;

    static char payload[96];

    /* wait for DHCP bind */
    ca_sem_wait(&s_dhcp_sem);

    /* On a point-to-point USB link the gateway IS the PC, which is also where
     * the broker runs -- so the broker address comes from the lease, exactly
     * as in ex42 on a LAN where the PC was the gateway too. */
    ca_ipv4_t broker_ip  = s_gw;
    ca_ipv4_t local_ip   = s_ip;

    ca_stdout_printf_n("MQTT broker %u.%u.%u.%u:%u",
        (u32)broker_ip.v[0], (u32)broker_ip.v[1],
        (u32)broker_ip.v[2], (u32)broker_ip.v[3],
        (u32)MQTT_BROKER_PORT);

    ca_mqtt_service_init(broker_ip, MQTT_BROKER_PORT,
                         local_ip, s_mac,
                         MQTT_CLIENT_ID,
                         on_mqtt_connected,
                         on_mqtt_message,
                         on_mqtt_disconnected);

    ca_stdout_put_n("MQTT: starting connection...");
    ca_mqtt_service_net_ready();

    u32 last_pub       = 0U;
    u32 last_ping      = 0U;
    u32 last_reconnect = ca_os_tick_ms();   /* avoids immediate double init */
    u32 pub_seq        = 0U;

    while (1) {
        u32 now = ca_os_tick_ms();

        if (!ca_mqtt_is_connected()) {
            if ((now - last_reconnect) >= MQTT_RECONNECT_MS) {
                last_reconnect = now;
                ca_stdout_put_n("MQTT: reconnect attempt...");
                /* update local IP in case of DHCP renewal */
                local_ip = s_ip;
                ca_mqtt_service_init(broker_ip, MQTT_BROKER_PORT,
                                     local_ip, s_mac,
                                     MQTT_CLIENT_ID,
                                     on_mqtt_connected,
                                     on_mqtt_message,
                                     on_mqtt_disconnected);
                ca_mqtt_service_net_ready();
            }
            ca_os_sleep(500U);
            continue;
        }

        /* publish status */
        if ((now - last_pub) >= MQTT_PUB_PERIOD_MS) {
            last_pub = now;
            ca_ipv4_t ip = s_ip;
            ca_snprintf(payload, sizeof(payload),
                "{\"seq\":%u,\"up\":%u,\"ip\":\"%u.%u.%u.%u\"}",
                pub_seq++,
                now / 1000U,
                (u32)ip.v[0], (u32)ip.v[1],
                (u32)ip.v[2], (u32)ip.v[3]);
            ca_mqtt_publish(MQTT_TOPIC_STATUS,
                            (const u8 *)payload,
                            (u16)ca_strlen(payload));
            ca_stdout_printf_n("MQTT pub -> %s", payload);
        }

        /* keepalive ping */
        if ((now - last_ping) >= MQTT_PING_PERIOD_MS) {
            last_ping = now;
            ca_mqtt_ping();
        }

        ca_os_sleep(200U);
    }
    return 0U;
}

/* ============================================================================
 * NCM link monitor — the NCM counterpart of task_eth_mon in ex42
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
        if (hb) { ca_led_on(EX77_LED_HB_A);  ca_led_off(EX77_LED_HB_B); }
        else    { ca_led_off(EX77_LED_HB_A); ca_led_on(EX77_LED_HB_B);  }
        ca_os_sleep(500);
    }
    return 0U;
}

/* Pool/stats diagnostics on request via CLI "net", "mqtt stat" and "tcp list":
 * the "eth" commands live inside #if EN_ETH and are not built on NCM. */

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

    /* Both LEDs are configured HERE, before any task runs. They are driven from
     * two different contexts -- the MQTT one from on_mqtt_message (MQTT service
     * task), the heartbeat from task_ncm_mon -- so leaving the setup inside
     * either task would let the other one toggle a pin that is not an output
     * yet, and the first command would land on an unconfigured GPIO. */
    ca_led_init();
    ca_led_off(EX77_LED_MQTT);
    /* initial state already alternating: A off, B on */
    ca_led_off(EX77_LED_HB_A);
    ca_led_on(EX77_LED_HB_B);

    /* ca_net_init() is NOT called here (unlike ex42): the NCM iface only exists
     * after the USB device is up, so both live in task_net.
     * ca_mqtt_service_init() is not here either -- it needs the broker address,
     * which comes from the DHCP lease, so it lives in task_mqtt (as in ex42). */

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
    ca_os_job("mqtt",      CA_TH_LEVEL3, task_mqtt,      NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("ncm_mon",   CA_TH_LEVEL3, task_ncm_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m77a, "=== Example 77 ===");
PRIVATE_STRING(m77b, "EN_NET / EN_USB / EN_USB_NCM / EN_MQTT_SERVICE / EN_TCP_SERVICE == 0: no tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m77a);
    PUT_N(&priv_m77b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_NET && EN_USB && EN_USB_NCM && EN_MQTT_SERVICE && ... */
