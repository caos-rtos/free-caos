// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 84 -- L2 echo: the driver measured with the transport taken out
 *
 * Hardware: NUCLEO-H753ZI / NUCLEO-H563ZI / NUCLEO-F767ZI (RMII, LAN8742A)
 *
 * WHY THIS EXAMPLE EXISTS
 *
 * ex44 measured iperf over Ethernet on the H753 and the two ends disagreed by
 * 13.6x: the PC sent 76,021,760 bytes at 60.7 Mbps and the board's iperf
 * counted 5,596,180 at 4.4 Mbps. The frames were NOT lost on the wire - the
 * driver counted 52,096 of them with zero errors, and 76,021,760 / 52,096 =
 * 1459.3, the MSS exactly. So every frame arrived and was handed up; the bytes
 * went missing somewhere ABOVE the driver.
 *
 * The trouble with measuring that with iperf is that iperf is TCP: driver,
 * net task, TCP receive ring and application all sit in the same number, and
 * a slow result does not say which of them is slow.
 *
 * This example removes everything above L2. A raw EtherType (0x88B5, which
 * IEEE reserves for exactly this kind of experimental use) is pulled out of
 * the receive path by the net filter, its source and destination MAC are
 * swapped, and it goes straight back out. No IP, no TCP, no receive ring, no
 * application waiting to be scheduled - just DMA in, DMA out.
 *
 * WHAT THE RESULT MEANS
 *
 *   echo keeps up with the sender  -> driver and net task are fine, and the
 *                                     13.6x is in the transport above them
 *   echo does NOT keep up          -> the limit is down here, and iperf was
 *                                     only ever showing it second-hand
 *
 * Either answer is worth having, which is what makes this a measurement
 * rather than a fix.
 *
 * STATIC IP, ON PURPOSE
 *
 * No DHCP, no TCP, no UDP services: the less that runs, the less can explain
 * away the number. ARP and ICMP echo stay on for one reason only - so that a
 * ping from the PC proves the board is on the network before any throughput
 * figure is believed. A board that does not answer a ping has not produced a
 * slow measurement, it has produced no measurement.
 *
 * Peer PC:
 *   sudo python3 examples/example_84/eth_echo_peer.py --iface <if> [--count N]
 *
 * REQUIREMENTS os_config.h:
 *   EN_ETH 1, EN_NET 1, EN_NET_FILTER 1, EN_ARP_SERVICE 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET && EN_NET_FILTER && EN_ARP_SERVICE

/* IEEE 802 local experimental EtherType 1. Nothing else on the network
 * answers it, so whatever comes back was echoed by this board. */
#define ECHO_ETHERTYPE   0x88B5U

/* Alternate function for the RMII pins. AF11 on every part that carries this
 * MAC (F7xx, H5xx, H7xx) - the examples each define it locally rather than
 * pulling in a header for one number. */
#define ETH_AF         11U

/* Static address: the bench PC is at 192.168.10.1 and its DHCP pool is
 * .50-.99, so .84 is reachable and cannot collide with a lease. */
#define BOARD_IP_A  192U
#define BOARD_IP_B  168U
#define BOARD_IP_C   10U
#define BOARD_IP_D   84U

static const ca_mac_t s_mac = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x84U } };
static ca_ipv4_t      s_ip;

/* Queue between the filter (runs in net task) and the echo task.
 * The filter must never block - the whole receive drain is behind it - so it
 * only hands the buffer over and returns. Depth is small on purpose: if the
 * echo task cannot keep up, frames should be dropped HERE and counted, not
 * quietly buffered until the pool runs dry. A drop that is counted is a
 * measurement; a drop that is hidden is a lie about the driver. */
#define ECHO_Q_DEPTH  16U
static u16         s_q[ECHO_Q_DEPTH];
static volatile u8 s_q_head, s_q_tail, s_q_cnt;
static ca_semaphore_t s_echo_sem;

/* Counters, printed by the 'echo' CLI command. */
static volatile u32 s_rx;          /* frames taken by the filter        */
static volatile u32 s_tx;          /* frames echoed back                */
static volatile u32 s_drop_q;      /* dropped: echo queue full          */
static volatile u32 s_drop_tx;     /* dropped: ca_net_send refused      */
static volatile u32 s_short;       /* dropped: shorter than an L2 header*/

/* ============================================================================
 * The filter: runs in net task context, owns the buffer, must not block
 * ============================================================================ */
static void echo_filter(u16 buf_idx)
{
    s_rx++;

    ca_net_buf_t *b = ca_net_buf_ptr(buf_idx);
    if (b->data_len < 14U) {          /* not even an Ethernet header */
        s_short++;
        ca_net_buf_free(buf_idx);
        return;
    }

    /* THE HANDOFF IS A CRITICAL SECTION, AND volatile IS NOT ENOUGH.
     *
     * "net" and "echo" are two tasks at the SAME level (CA_TH_LEVEL6), so they
     * preempt each other at every scheduler quantum. s_q_cnt++ here and
     * s_q_cnt-- in task_echo are read-modify-write (LDRB/ADD/STRB on M33):
     * volatile makes the compiler reload the variable, it does NOT make the
     * update atomic. A preemption between the load and the store loses one
     * update, and then head and tail disagree with cnt.
     *
     * What that looked like on the H563 on 2026-09-17, after a 20000-frame
     * burst at 100 Mbps: the echo task read a slot it had already consumed, so
     * the SAME buf_idx went out twice - 11 to 31 duplicate sequence numbers
     * per run reported by the generator - and was freed twice, which is the
     * origin of the 56 refused double frees. It also explains drop_q=0 with a
     * queue that was supposedly never full: a corrupted count never reaches
     * ECHO_Q_DEPTH.
     *
     * The chain does not end there. A buffer freed while still queued goes
     * back to the pool, gets reallocated, and ca_net_buf_alloc() clears its
     * data_len under the first owner's feet - which is how a descriptor with
     * length 0 reached the DMA and hung it with AXWHSTS=1. See
     * [[project_eth_rx_ring_stall]].
     *
     * PRIMASK save/restore, the same idiom net_buf.c uses: it nests, and the
     * section is three stores long. */
    u32 primask = __get_PRIMASK();
    __disable_irq();

    if (s_q_cnt >= ECHO_Q_DEPTH) {    /* echo task is behind: drop and say so */
        __set_PRIMASK(primask);
        s_drop_q++;
        ca_net_buf_free(buf_idx);
        return;
    }

    s_q[s_q_tail] = buf_idx;
    s_q_tail = (u8)((s_q_tail + 1U) % ECHO_Q_DEPTH);
    s_q_cnt++;

    __set_PRIMASK(primask);

    ca_sem_post_capped(&s_echo_sem);
}

/* ============================================================================
 * Echo task -- swaps src/dst MAC and sends the SAME buffer back (zero copy)
 * ============================================================================ */
static u32 task_echo(void *arg)
{
    (void)arg;

    for (;;) {
        ca_sem_wait(&s_echo_sem);

        /* Drain everything queued per wakeup: the semaphore is capped, so a
         * wakeup means "there is work", not "there is exactly one frame". */
        for (;;) {
            /* Take one slot under the same lock the filter uses: the dequeue
             * is read-modify-write too, and the test on s_q_cnt has to be part
             * of it - checking outside and taking inside would let the filter
             * empty the queue in between. See the note in echo_filter(). */
            u32 primask = __get_PRIMASK();
            __disable_irq();

            if (s_q_cnt == 0U) {
                __set_PRIMASK(primask);
                break;
            }

            u16 idx = s_q[s_q_head];
            s_q_head = (u8)((s_q_head + 1U) % ECHO_Q_DEPTH);
            s_q_cnt--;

            __set_PRIMASK(primask);

            ca_net_buf_t *b   = ca_net_buf_ptr(idx);
            u8           *frm = b->data + b->data_off;

            /* Swap destination and source MAC in place. The frame goes back
             * out of the very buffer the DMA wrote it into - no copy, which is
             * the whole point: what is being measured is the driver, not
             * memcpy. */
            for (u8 i = 0U; i < 6U; i++) {
                u8 t       = frm[i];
                frm[i]     = frm[i + 6U];
                frm[i + 6U] = t;
            }

            /* ca_net_send owns the buffer from here, success or not. */
            if (ca_net_send(idx)) s_tx++;
            else                  s_drop_tx++;
        }
    }
    return 0U;
}

/* ============================================================================
 * CLI: 'echo' -- the counters this example exists to produce
 * ============================================================================ */
#if ENABLE_CLI
static bool echo_cli_handler(c_str *pcmd)
{
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("echo      : stats | clear");
        return false;
    }
    if (ca_strncmp(p, "echo", 4) != 0) return false;
    p = ca_skip_sp(p + 4);

    if (ca_strncmp(p, "stats", 5) == 0) {
        CLI_PRINTF_N("echo rx=%u tx=%u", (u32)s_rx, (u32)s_tx);
        CLI_PRINTF_N("drop q=%u tx=%u short=%u",
                     (u32)s_drop_q, (u32)s_drop_tx, (u32)s_short);
        CLI_PRINTF_N("filter took %u", ca_net_filter_count());
        /* rx and tx must agree. A gap is this example's own doing - the queue
         * overflowed or the send refused - and both are counted above, so the
         * driver is never blamed for a frame this code dropped itself. */
        CLI_PUT_N((s_rx == s_tx) ? "balanced" : "GAP: rx != tx");
        return true;
    }
    if (ca_strncmp(p, "clear", 5) == 0) {
        s_rx = 0U; s_tx = 0U; s_drop_q = 0U; s_drop_tx = 0U; s_short = 0U;
        CLI_PUT_N("echo counters cleared");
        return true;
    }

    CLI_PUT_N("echo      : stats | clear");
    return false;
}
static ca_cli_node_t s_echo_cli_node = { echo_cli_handler, NULL };
#endif

/* ============================================================================
 * Net
 * ============================================================================ */
static const ca_net_cfg_t net_cfg = {
    .ip   = { .addr = 0U },           /* set from s_ip in task_net */
    .mac  = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x84U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },   /* so a ping can prove the board is up */
};

static ca_eth_cfg_t eth_cfg = {
    .mac       = { .v = { 0x02U,0x00U,0x00U,0x00U,0x00U,0x84U } },
    .phy_addr  = 0U,
    .rmii      = true,
    .rx_cb     = NULL,
    .tx_done_cb= NULL,
    .error_cb  = NULL,
    .link_cb   = NULL,
};

static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_put_n("\n\n=== EX84: L2 echo (EtherType 0x88B5) ===");
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0], s_mac.v[1], s_mac.v[2],
        s_mac.v[3], s_mac.v[4], s_mac.v[5]);
    ca_stdout_printf_n("IP   %u.%u.%u.%u  (static)",
        BOARD_IP_A, BOARD_IP_B, BOARD_IP_C, BOARD_IP_D);

    ca_net_proto_init(&net_cfg);
    ca_net_proto_set_local_ip(s_ip);

    ca_eth_link_poll();
    ca_os_sleep(200);

    ca_net_filter_set(ECHO_ETHERTYPE, echo_filter);
    ca_stdout_put_n("filter armed on 0x88B5 - ping to check the board is up,"
                    " then run eth_echo_peer.py");

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) {
        ca_eth_link_poll();
        ca_led_toggle(3U);
        ca_os_sleep(500);
    }
    return 0U;
}

/* ============================================================================
 * GPIO ETH — RMII
 * ============================================================================ */
static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
        /* RMII_TXD1: the H563 routes it to PB15 (UM3115, JP6 ON / SB10 OFF),
         * the H753 and F767 to PB13. Hardcoding PB13 leaves TXD1 unconnected
         * on an H563: every frame still counts as transmitted and tx_fail
         * stays 0, but half the data bits never reach the PHY. */
#if MCU_ID == 563
        { CA_GPIO_PORT_B, 15U },
#else
        { CA_GPIO_PORT_B, 13U },
#endif
    };
    for (u32 i = 0U; i < sizeof(pins)/sizeof(pins[0]); i++) {
        ca_gpio_configure(pins[i].port, pins[i].pin,
            CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
            CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, ETH_AF);
    }
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
        ca_cli_register(&s_echo_cli_node);
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    s_ip.v[0] = BOARD_IP_A;  s_ip.v[1] = BOARD_IP_B;
    s_ip.v[2] = BOARD_IP_C;  s_ip.v[3] = BOARD_IP_D;

    ca_sem_init(&s_echo_sem, "SECHO", 0U, 8U);

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");

    ca_arp_service_init(s_mac, s_ip);
}

void ca_os_main(void)
{
    ca_arp_service_start();

    /* The echo task at the same level as the net task: the frame is handed
     * from one to the other and both are on the receive path, so giving
     * either a larger share would only move the queue, not shorten it. */
    ca_os_job("net",     CA_TH_LEVEL6, task_net,     NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("echo",    CA_TH_LEVEL6, task_echo,    NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m84a, "=== Example 84: L2 echo (EtherType 0x88B5) ===");
PRIVATE_STRING(m84b, "DISABLED - requires EN_ETH / EN_NET / EN_NET_FILTER / EN_ARP_SERVICE");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m84a);
    PUT_N(&priv_m84b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET && EN_NET_FILTER && EN_ARP_SERVICE */
