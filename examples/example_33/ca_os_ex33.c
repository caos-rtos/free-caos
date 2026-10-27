// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 33 -- caOS Net Stack: buffer pool + ARP/ICMP/UDP via net_proto
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI) or NUCLEO-H563ZI (STM32H563ZI)
 *           PHY: LAN8742A (RMII, addr 0)
 *           RMII_TXD1: PB13 (H753) / PB15 (H563, UM3115 — JP6 ON)
 *
 * Functionality:
 *   1. Pool @ 0x30000000 D2 SRAM1 NCacheable (H753) / 0x20000000 internal SRAM (H563)
 *   2. Self-test: alloc 64, verify exhaustion, free 64
 *   3. Automatic ARP reply via ca_net_proto_init()
 *   4. ICMP echo reply (ping) via ca_net_proto_init()
 *   5. Stats every 5 s: rx/drop/leak check
 *
 * PC Procedure:
 *   sudo ip addr add 192.168.10.1/24 dev eth0
 *   arping -c 10 -I eth0 192.168.10.33
 *   ping -c 5 192.168.10.33
 *
 * REQUIREMENTS os_config.h: EN_ETH 1, EN_NET 1
 */

#include "caos.h"
#include "net_proto.h"

#if EN_ETH && EN_NET

/* ============================================================================
 * Hardware
 * ============================================================================ */

#if MCU_ID == 563
#else
#endif

#define ETH_AF         11U

/* ============================================================================
 * Network
 * ============================================================================ */
static const ca_mac_t  s_mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x33U } };
static const ca_ipv4_t s_ip  = { .v = { 192U, 168U, 10U, 33U } };

static const ca_net_cfg_t net_cfg = {
    .ip  = { .v = { 192U, 168U, 10U, 33U } },
    .mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x33U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x33U } },
    .link_cb  = on_link_change,
    /* rx_cb / error_cb: overwritten by ca_net_init() */
};

/* ============================================================================
 * Pool self-test
 *   Allocate all buffers, verify exhaustion, free everything.
 *   Print PASS/FAIL for each step.
 * ============================================================================ */
static void pool_selftest(void)
{
    ca_stdout_printf_n("--- pool selftest ---");

    /* Step 1: physical address of the pool. The right region differs per part:
     *   H7xx  D2-SRAM1 non-cacheable      0x30000000
     *   H5xx  internal SRAM (M33), DMA-coherent, section .net_pool
     *   F7xx  SRAM1 (F767 0x20020000, F746 0x20010000): it MUST stay outside
     *         the TCM, which the DMAs cannot reach. The D-cache is off on M7, so
     *         no non-cacheable region is needed.
     * The generic branch checked 0x30000000, i.e. the H7's D2: on the F767 the
     * pool sits at 0x20020xxx and the selftest said "WRONG REGION!" WRONGLY,
     * with the right memory (09/09). A check that blames the board instead of
     * itself is worse than no check. */
    u32 addr = (u32)ca_net_buf_ptr(0);
#if MCU_ID == 563
    bool region_ok = (addr >= 0x20000000U && addr < 0x20080000U);
    ca_stdout_printf_n("  pool[0] @ 0x%08X  %s", addr,
        region_ok ? "internal SRAM OK" : "WRONG REGION!");
#elif MCU_ID == 767 || MCU_ID == 746
    /* outside the TCM: the DTCM ends at 0x20020000 on the F767, 0x20010000
     * on the F746, and above it there is SRAM1/SRAM2 up to 0x2007C000+16K. */
    ca_stdout_printf_n("  pool[0] @ 0x%08X  %s", addr,
        (addr >= 0x20010000U && addr < 0x20080000U) ? "SRAM1 OK" : "WRONG REGION!");
#else
    ca_stdout_printf_n("  pool[0] @ 0x%08X  %s", addr,
        (addr >= 0x30000000U && addr < 0x30020000U) ? "D2-SRAM1 OK" : "WRONG REGION!");
#endif

    /* Step 2: allocate every buffer the pool can still give.
     * The selftest runs AFTER ca_eth_init(), which has already loaded ETH_RX_N
     * buffers into the RX descriptors (8 on H753, 4 on the other families), so
     * the pool is NOT pristine here: expect NET_BUF_COUNT - held, not the full
     * count. Asking for all 64 reported a bogus "FAIL (short)" on H753.
     *
     * The count must come from the driver, not from an assumption: on the H563
     * ca_eth_hw_pool_held_count() answered 0 (a leftover from the copy-based
     * driver) and the four armed RX slots looked like a leak. Fixed 2026-08-28.
     * Every line below prints the full arithmetic - free + held out of
     * NET_BUF_COUNT - so a short pool cannot be mistaken for a small one. */
    u16 held     = ca_eth_pool_held_count();
    u16 expected = (u16)(NET_BUF_COUNT - held);
    u16 saved[NET_BUF_COUNT];
    u16 n = 0U;
    for (; n < NET_BUF_COUNT; n++) {
        saved[n] = ca_net_buf_alloc();
        if (saved[n] == NET_BUF_INVALID) break;
    }
    ca_stdout_printf_n("  alloc %u/%u available (%u total - %u held by driver): %s",
        (u32)n, (u32)expected, (u32)NET_BUF_COUNT, (u32)held,
        n == expected ? "PASS" : "FAIL (short)");

    /* Step 3: with the pool drained, the next alloc must refuse */
    u16 extra = ca_net_buf_alloc();
    ca_stdout_printf_n("  alloc when empty refused: %s",
        extra == NET_BUF_INVALID ? "PASS" : "FAIL got a valid index!");

    /* Step 4: free everything — the pool must return to what it was before the
     * test, i.e. the free count minus the buffers the driver legitimately holds.
     * A real leak is free + held < NET_BUF_COUNT. */
    for (u16 i = 0U; i < n; i++) ca_net_buf_free(saved[i]);
    u16 after = ca_net_buf_free_count();
    ca_stdout_printf_n("  free_count after restore: %u free + %u held = %u/%u  %s",
        (u32)after, (u32)held, (u32)(after + held), (u32)NET_BUF_COUNT,
        (after + held) == NET_BUF_COUNT ? "PASS" : "FAIL leak detected!");

    /* Step 5: verify unique indices (no duplicates in the free list).
     * Alloc everything available and mark each index in a bitmap: an index
     * handed out twice means the free list is corrupt. Buffers held by the
     * driver simply never show up here. */
    u8 seen[NET_BUF_COUNT / 8U + 1U];
    for (u16 i = 0U; i < (u16)sizeof(seen); i++) seen[i] = 0U;
    bool dup = false;
    u16 allocs[NET_BUF_COUNT];
    for (u16 i = 0U; i < NET_BUF_COUNT; i++) {
        allocs[i] = ca_net_buf_alloc();
        if (allocs[i] < NET_BUF_COUNT) {
            u16 byte = allocs[i] / 8U;
            u8  bit  = (u8)(1U << (allocs[i] % 8U));
            if (seen[byte] & bit) { dup = true; break; }
            seen[byte] |= bit;
        }
    }
    for (u16 i = 0U; i < NET_BUF_COUNT; i++) {
        if (allocs[i] != NET_BUF_INVALID) ca_net_buf_free(allocs[i]);
    }
    ca_stdout_printf_n("  unique indices: %s", dup ? "FAIL dup!" : "PASS");
    ca_stdout_printf_n("--- pool selftest done  free=%u + %u held = %u ---",
        (u32)ca_net_buf_free_count(), (u32)ca_eth_pool_held_count(),
        (u32)(ca_net_buf_free_count() + ca_eth_pool_held_count()));
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static u32 task_net(void *arg)
{
    (void)arg;

    EVAL_USB_CDC_STD(ca_usb_cdc_wait_connected(5000);)
    ca_os_sleep(300);

    ca_stdout_printf_n("\n\n=== EX33: caOS net stack ===");
    ca_stdout_printf_n("Pool: %u bufs x %u B = %u KB",
        (u32)NET_BUF_COUNT, (u32)NET_BUF_SIZE,
        (u32)(NET_BUF_COUNT * NET_BUF_SIZE / 1024U));
    ca_stdout_printf_n("MAC  %02X:%02X:%02X:%02X:%02X:%02X",
        s_mac.v[0],s_mac.v[1],s_mac.v[2],s_mac.v[3],s_mac.v[4],s_mac.v[5]);
    ca_stdout_printf_n("IP   %u.%u.%u.%u",
        s_ip.v[0],s_ip.v[1],s_ip.v[2],s_ip.v[3]);

    pool_selftest();

    ca_net_proto_init(&net_cfg);

    ca_os_sleep(500);
    ca_eth_link_poll();
    ca_arp_send_garp();
    ca_os_sleep(50);

    ca_stdout_printf_n("Net task ready -- waiting frames");

    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0;
}

/* Stats/pool/leak diagnostics: available on request via CLI "eth stats" |
 * "eth pool" | "eth dump" (caos/drivers/src/eth.c) — no periodic task here. */

/* ============================================================================
 * Link callback / monitor
 * ============================================================================ */
static void on_link_change(bool up)
{
    if (up) ca_led_on(1U); else ca_led_off(1U);
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) {
        ca_eth_link_poll();
        ca_os_sleep(500);
    }
    return 0;
}

/* ============================================================================
 * Heartbeat
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
 * GPIO ETH RMII (AF11)
 * ============================================================================ */
static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
        /* RMII_TXD1: NUCLEO-H563ZI (UM3115) uses PB15 (JP6 ON); NUCLEO-H753ZI uses PB13 */
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
    )
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    
    ca_led_off(1U);
    ca_led_init();
    ca_led_off(3U);

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg))) {
        CA_SAFE_FAIL("ca_net_init");
    }

    /* ARP replies come from net_proto's own handler, so the wire works even
     * without this - which is why its absence went unnoticed. What was missing
     * is the SERVICE: the cache, and the node that puts "arp" in the CLI
     * (arp_service.c registers it here, under EN_CLI_ARP). ca_arp_send_garp()
     * in task_net was also being called on an uninitialised service.
     * Unlike the DHCP examples, the address is known at compile time here, so
     * it is passed straight in rather than as 0.0.0.0 to be filled in later. */
    ca_arp_service_init(s_mac, s_ip);
}

void ca_os_main(void)
{
    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else


/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m33a, "=== Example 33 ===");
PRIVATE_STRING(m33b, "EN_ETH/EN_NET == 0: this part has no MAC, or ETH is off");
void ca_os_configure(void)
{
    PUT_N(&priv_m33a);
    /* The line the bench matches on - fixed everywhere. The reason follows. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m33b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_ETH && EN_NET */
