/*
 * app_2 - caOS bootloader (NUCLEO-H563ZI, NUCLEO-H753ZI).
 *
 * Guide: workspace/app_2/doc/app_2_user_guide.md. At reset, before any task exists, the
 * bootloader picks a slot, checks its image (header + SHA-256, boot_slot.c)
 * and hands the CPU over with ca_os_end(): the image starts as after a reset
 * and sets up clocks and everything else by itself.
 *
 *   slot      the one in the boot configuration; with no configuration, the
 *             first valid of A and B; an invalid active slot falls back to
 *             the other one when that one is valid
 *   trial     a pending slot (an update) starts with the IWDG armed and the
 *             tries counted in a backup register (caos/inc/boot_image.h);
 *             the image confirms and becomes the active one at the next
 *             reset, or after CA_BOOT_MAX_TRIES starts it is rolled back
 *             (erased) and the active one starts again
 *   recovery  no valid image, or B1 held at reset: the bootloader stays up
 *             with its CLI (boot info | select | go | erase) and the network
 *             (DHCP): a .cimg pushed over TFTP (tools/caos_tftp put <ip>
 *             <file.cimg>) or written over USB DFU (dfu-util -D <file.cimg>)
 *             goes to the free slot, is checked and started as a trial
 *
 * Images: an app with image.cmake builds <mcu>_<app>_slotA/_slotB.elf;
 * tools/caos_image.py turns one into a .cimg, written at the slot base.
 *
 * Config: os_config_app_2.h (caos_config.py, never by hand).
 */
#include "caos.h"
#include "net_proto.h"
#include "boot_slot.h"
#include "boot_pubkey.h"

/* B1 USER: PC13 on both boards (UM3115, UM2407), active high */
#define BTN_PORT        CA_GPIO_PORT_C
#define BTN_PIN         13U

#if BOOT_SUPPORTED

static const char *s_recovery_why = "";

static const char *slot_name(char s)
{
    return s == 'A' ? "A" : s == 'B' ? "B" : "-";
}

/* Power-on, not a reset: the ST-LINK VCP enumerates late, so the bootloader
 * waits for it before printing anything. The flag is left set for the image. */
static bool power_on(void)
{
#if MCU_ID == 753
    return (RCC->RSR & RCC_RSR_PORRSTF) != 0U;
#else
    return (RCC->RSR & RCC_RSR_BORRSTF) != 0U;   /* H5: power-on is a BOR */
#endif
}

#define BOOT_STR_(x) #x
#define BOOT_STR(x)  BOOT_STR_(x)

/* The bootloader's own banner, in the style of the caOS one. Printed before
 * the scheduler (ca_os_banner() is a task, and the normal path leaves first). */
static void boot_banner(bool held, char *buf)
{
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "\r\n=================================================");
    PRINTF_N(buf, DEFAULT_MSG_SIZE, " -- c.a.O.S bootloader --");
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   build : %s", BUILD_VER);
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   MCU : STM 32 F:%s T:%s", BOOT_STR(MCU), BOOT_STR(MCU_ID));
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "=================================================");
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   System Clock: %u Hz", SystemCoreClock);
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   Slot A: 0x%08X  Slot B: 0x%08X  (%u KB each)",
             BOOT_SLOT_A_BASE, BOOT_SLOT_B_BASE, BOOT_SLOT_LEN / 1024U);
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   Boot conf: 0x%08X", BOOT_CONF_ADDR);
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   Images: %s, key %s",
             BOOT_REQUIRE_SIGNED ? "signed only" : "signed or unsigned", BOOT_PUBKEY_ID);
    const int wrp = boot_wrp_state();
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   Bootloader area: %s", wrp == 1 ? "write protected" :
             wrp == 0 ? "NOT write protected ('boot protect on')" : "PARTLY write protected");
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "   B1: %s", held ? "held - recovery" : "released");
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "=================================================");
}

/* The slot to start: the configured one, else (none chosen, or not valid)
 * the first valid, saying why. 0 = none, *why tells the reason. */
static char pick_slot(boot_hdr_t *hdr, const char **why, char *buf)
{
    boot_conf_t conf;
    char first = boot_conf_read(&conf) ? conf.active : 0;
    if (first == 'A' || first == 'B') {
        *why = boot_slot_check(first, hdr);
        if (*why == NULL) return first;
        PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: configured slot %s not valid (%s)",
                 slot_name(first), *why);
    } else {
        PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: no slot configured, first valid one");
    }
    for (char s = 'A'; s <= 'B'; s++) {
        if (s == first) continue;
        if (boot_slot_check(s, hdr) == NULL) return s;
    }
    *why = "no valid image in A or B";
    return 0;
}

/* Messages left by an image (confirm, update request), then the pending
 * slot: its tries, or the rollback. Returns the slot to start as a trial,
 * 0 to go on with the normal choice. */
static char boot_trial(boot_hdr_t *hdr, char *buf)
{
    boot_conf_t c;
    const bool have = boot_conf_read(&c);
    char active  = have ? c.active  : 0;
    char pending = have ? c.pending : 0;

    /* 1. a confirmed trial becomes the active slot */
    const u32 st0 = ca_bkp_read(CA_BOOT_BKP_STATE);
    if (pending != 0 && st0 == CA_BOOT_STATE(pending, CA_BOOT_CONFIRMED)) {
        if (boot_conf_write(pending, 0)) {
            PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: slot %s confirmed, now the active one",
                     slot_name(pending));
            active = pending;
            pending = 0;
        }
        (void)ca_bkp_write(CA_BOOT_BKP_STATE, 0U);
    }

    /* 2. an update request from the image */
    const u32 msg = ca_bkp_read(CA_BOOT_BKP_MSG);
    (void)ca_bkp_write(CA_BOOT_BKP_MSG, 0U);
    if ((msg & CA_BOOT_MAGIC_MASK) == CA_BOOT_MAGIC) {
        const char cmd = (char)((msg >> 8) & 0xFFU);
        const char s   = (char)(msg & 0xFFU);
        if (cmd == CA_BOOT_MSG_UPDATE && (s == 'A' || s == 'B') && s != active) {
            const char *why = boot_slot_check(s, hdr);
            if (why) {
                PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: update of slot %s refused: %s",
                         slot_name(s), why);
            } else if (boot_conf_write(active, s)) {
                PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: update - slot %s v%u pending",
                         slot_name(s), hdr->version);
                pending = s;
                (void)ca_bkp_write(CA_BOOT_BKP_STATE, 0U);
            }
        }
    }
    if (pending != 'A' && pending != 'B') {
        (void)ca_bkp_write(CA_BOOT_BKP_STATE, 0U);
        return 0;
    }

    const u32 st = ca_bkp_read(CA_BOOT_BKP_STATE);
    u32 tries = ((st & CA_BOOT_MAGIC_MASK) == CA_BOOT_MAGIC &&
                 (char)((st >> 8) & 0xFFU) == pending) ? (st & 0xFFU) : 0U;
    tries++;
    const char *why = boot_slot_check(pending, hdr);
    if (why || tries > CA_BOOT_MAX_TRIES) {
        /* Rolled back: erased, so that no later choice starts it unwatched */
        PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: ROLLBACK - slot %s %s, erased; back to slot %s",
                 slot_name(pending), why ? why : "never confirmed", slot_name(active));
        (void)boot_conf_write(active, 0);
        (void)ca_bkp_write(CA_BOOT_BKP_STATE, 0U);
        ca_flash_init();
        (void)boot_slot_erase(pending);
        return 0;
    }
    (void)ca_bkp_write(CA_BOOT_BKP_STATE, CA_BOOT_STATE(pending, tries));
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: trial of slot %s, start %u of %u, IWDG %u ms",
             slot_name(pending), tries, CA_BOOT_MAX_TRIES, CA_BOOT_IWDG_MS);
    return pending;
}

static void boot_slot(char s, const boot_hdr_t *hdr, char *buf)
{
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: slot %s v%u \"%s\" -> 0x%08X",
             slot_name(s), hdr->version, hdr->tag, hdr->link_addr);
    ca_os_end(hdr->link_addr);                 /* does not return */
}

static void print_slot(char s)
{
    boot_hdr_t h;
    const char *why = boot_slot_check(s, &h);
    if (why)
        CLI_PRINTF_N("slot %s 0x%08X: %s", slot_name(s), boot_slot_base(s), why);
    else
        CLI_PRINTF_N("slot %s 0x%08X: VALID v%u \"%s\", %u bytes, %s", slot_name(s),
                     boot_slot_base(s), h.version, h.tag, h.image_size,
                     (h.flags & BOOT_HDR_SIGNED) ? "signed" : "unsigned");
}

static char parse_slot(const char *p)
{
    p = ca_skip_sp(p);
    if (*p == 'A' || *p == 'a') return 'A';
    if (*p == 'B' || *p == 'b') return 'B';
    return 0;
}

static bool boot_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("boot      : info | select A|B | go [A|B] | erase A|B | protect [on|off]  (bootloader)");
        return false;
    }
    if (ca_strncmp(p, "boot", 4) != 0) return false;
    p = ca_skip_sp(p + 4);

    if (*p == '\0' || ca_strncmp(p, "info", 4) == 0) {
        boot_conf_t c;
        if (boot_conf_read(&c))
            CLI_PRINTF_N("conf: active %s, pending %s (written %u times)",
                         slot_name(c.active), slot_name(c.pending), c.seq);
        else
            CLI_PUT_N("conf: none (the first valid slot starts)");
        print_slot('A');
        print_slot('B');
        if (*s_recovery_why) CLI_PRINTF_N("recovery: %s", s_recovery_why);
    } else if (ca_strncmp(p, "select", 6) == 0) {
        char s = parse_slot(p + 6);
        boot_hdr_t h;
        const char *why = s ? boot_slot_check(s, &h) : "slot A or B";
        if (why)                         CLI_PRINTF_N("boot select: %s", why);
        else if (boot_conf_write(s, 0))  CLI_PRINTF_N("boot select: slot %s starts at reset", slot_name(s));
        else                             CLI_PUT_N("boot select: writing the configuration FAILED");
    } else if (ca_strncmp(p, "go", 2) == 0) {
        boot_conf_t c;
        char s = parse_slot(p + 2);
        if (s == 0 && boot_conf_read(&c)) s = c.active;
        boot_hdr_t h;
        const char *why = s ? boot_slot_check(s, &h) : "no slot given and none configured";
        if (why) {
            CLI_PRINTF_N("boot go: %s", why);
        } else {
            CLI_PRINTF_N("boot go: slot %s v%u -> 0x%08X", slot_name(s), h.version, h.link_addr);
            (void)ca_os_sleep(120);            /* let the drain thread get it out */
            ca_os_end(h.link_addr);
        }
    } else if (ca_strncmp(p, "protect", 7) == 0) {
        p = ca_skip_sp(p + 7);
        if (ca_strncmp(p, "on", 2) == 0 || ca_strncmp(p, "off", 3) == 0) {
            const bool on = (p[1] == 'n');
            CLI_PRINTF_N("boot protect: %s", boot_wrp_set(on) ?
                         (on ? "bootloader area write protected (option bytes)" :
                               "protection removed") : "option byte change FAILED");
        }
        const int w = boot_wrp_state();
        CLI_PRINTF_N("boot protect: %s", w == 1 ? "ON - SWD cannot rewrite the bootloader either" :
                     w == 0 ? "off" : "PARTLY on");
    } else if (ca_strncmp(p, "erase", 5) == 0) {
        char s = parse_slot(p + 5);
        if (s == 0)                  CLI_PUT_N("boot erase: slot A or B");
        else if (boot_slot_erase(s)) CLI_PRINTF_N("boot erase: slot %s erased", slot_name(s));
        else                         CLI_PRINTF_N("boot erase: slot %s FAILED", slot_name(s));
    } else {
        CLI_PUT_N("boot: info | select A|B | go [A|B] | erase A|B | protect [on|off]");
    }
    return true;
}

static ca_cli_node_t s_boot_node = { boot_cli, NULL };

/* =============================================================================
 * Network (recovery): DHCP, ARP, TFTP server for the update
 * ============================================================================= */
#define ETH_AF  11U

static const ca_mac_t s_mac = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0xB0U, 0x07U } };

static void on_link_change(bool up)
{
    ca_stdout_printf_n("ETH LINK %s", up ? "UP" : "DOWN");
}

static ca_eth_cfg_t s_eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .mac      = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0xB0U, 0x07U } },
    .link_cb  = on_link_change,
};

static const ca_net_cfg_t s_net_cfg = {
    .ip   = { .addr = 0U },
    .mac  = { .v = { 0x02U, 0x00U, 0x00U, 0x00U, 0xB0U, 0x07U } },
    .arp  = { .renew_ms = 30000U },
    .udp  = { .bindings = { {0U, NULL} }, .count = 0U },
    .icmp = { .echo_reply = true },
};

static void dhcp_udp_handler(u16 buf_idx)
{
    ca_net_buf_t *b = ca_net_buf_ptr(buf_idx);
    u8  *frame  = b->data + b->data_off;
    u16  l4_off = b->meta.rx.l4_off;
    ca_dhcp_service_rx_handler((const ca_udp_frame_t *)(frame + l4_off),
                               (u16)(b->data_len - l4_off));
    ca_net_buf_free(buf_idx);
}

static void on_dhcp_bound(ca_ipv4_t ip, ca_ipv4_t mask, ca_ipv4_t gw, ca_ipv4_t dns)
{
    (void)mask; (void)gw; (void)dns;
    ca_net_proto_set_local_ip(ip);
    ca_arp_service_update_local_ip(ip);
    ca_arp_send_garp();
    ca_stdout_printf_n("BOOT: update - tools/caos_tftp put %u.%u.%u.%u <image.cimg>",
                       ip.v[0], ip.v[1], ip.v[2], ip.v[3]);
}

static void on_dhcp_expire(void)
{
    ca_stdout_put_n("BOOT: DHCP lease expired");
}

static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
#if MCU_ID == 563
        { CA_GPIO_PORT_B, 15U },     /* RMII_TXD1: PB15 on NUCLEO-H563ZI (UM3115) */
#else
        { CA_GPIO_PORT_B, 13U },     /* PB13 on NUCLEO-H753ZI */
#endif
    };
    for (u32 i = 0U; i < sizeof(pins) / sizeof(pins[0]); i++)
        ca_gpio_configure(pins[i].port, pins[i].pin, CA_GPIO_MODE_AF, CA_GPIO_OTYPE_PP,
                          CA_GPIO_SPEED_VERY_HIGH, CA_GPIO_NOPULL, ETH_AF);
}

static u32 task_net(void *arg)
{
    (void)arg;
    ca_net_proto_init(&s_net_cfg);
    ca_net_proto_bind_udp(DHCP_CLIENT_PORT, dhcp_udp_handler);
    ca_eth_link_poll();
    ca_os_sleep(200);
    ca_dhcp_service_net_ready();
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0U;
}

static u32 task_eth_mon(void *arg)
{
    (void)arg;
    while (1) { ca_eth_link_poll(); ca_os_sleep(500); }
    return 0U;
}

/* =============================================================================
 * Update: one .cimg pushed over TFTP into the slot not in use
 * ============================================================================= */
typedef struct {
    char slot;           /* destination */
    u32  bytes;
    bool flash_err;
} upd_t;

/* The slot an update may overwrite: the one that would NOT start at reset
 * (the configured one when valid, else the first valid). Both empty: A. */
static volatile bool s_upd_busy;     /* one update at a time: TFTP or USB DFU */

static char free_slot(void)
{
    boot_conf_t c;
    boot_hdr_t h;
    char run = 0;
    if (boot_conf_read(&c) && (c.active == 'A' || c.active == 'B') &&
        boot_slot_check(c.active, &h) == NULL)
        run = c.active;
    else if (boot_slot_check('A', &h) == NULL)
        run = 'A';
    else if (boot_slot_check('B', &h) == NULL)
        run = 'B';
    return run == 'A' ? 'B' : 'A';
}

static bool ends_with(const char *s, const char *tail)
{
    u32 n = (u32)ca_strlen(s), t = (u32)ca_strlen(tail);
    return n >= t && ca_strncmp(s + n - t, tail, t) == 0;
}

/* A received image: checked, and made pending (true) for its trial */
static bool upd_make_pending(char slot, u32 bytes)
{
    boot_hdr_t h;
    const char *why = boot_slot_check(slot, &h);
    if (why) {
        ca_stdout_printf_n("BOOT: update received, %u bytes, slot %s NOT valid: %s",
                           bytes, slot_name(slot), why);
        return false;
    }
    boot_conf_t c;
    const char active = boot_conf_read(&c) ? c.active : 0;
    const bool ok = boot_conf_write(active, slot);
    ca_stdout_printf_n("BOOT: update OK, slot %s v%u \"%s\", %s - %s",
                       slot_name(slot), h.version, h.tag,
                       (h.flags & BOOT_HDR_SIGNED) ? "signed" : "unsigned",
                       ok ? "pending, reset into its trial" : "writing the conf FAILED");
    return ok;
}

static u16 upd_on_wrq(const char *name, ca_ipv4_t from, void *arg)
{
    upd_t *u = (upd_t *)arg;
    if (s_upd_busy) {
        ca_stdout_printf_n("BOOT: update \"%s\" refused - another update is running", name);
        return TFTP_ERR_ACCESS;
    }
    if (!ends_with(name, ".cimg")) {
        ca_stdout_printf_n("BOOT: update \"%s\" refused - not a .cimg", name);
        return TFTP_ERR_ACCESS;
    }
    s_upd_busy = true;
    u->slot = free_slot();
    u->bytes = 0U;
    u->flash_err = false;
    ca_stdout_printf_n("BOOT: update \"%s\" from %u.%u.%u.%u -> slot %s, erasing ...",
                       name, from.v[0], from.v[1], from.v[2], from.v[3], slot_name(u->slot));
    if (!boot_slot_erase(u->slot)) {
        ca_stdout_printf_n("BOOT: erase of slot %s FAILED", slot_name(u->slot));
        s_upd_busy = false;
        return TFTP_ERR_DISKFULL;
    }
    return 0U;
}

static bool upd_on_block(u16 blk, const u8 *data, u16 len, void *arg)
{
    upd_t *u = (upd_t *)arg;
    const u32 off = (u32)(blk - 1U) * TFTP_BLOCK_SIZE;   /* < 65536 blocks: the slot is smaller */
    if (off + len > BOOT_SLOT_LEN) return false;
    if (len == 0U) return true;
    bool ok = ca_flash_unlock() && ca_flash_write(boot_slot_base(u->slot) + off, data, len);
    ca_flash_lock();
    if (!ok) { u->flash_err = true; return false; }
    u->bytes = off + len;
    return true;
}

static u32 task_update(void *arg)
{
    (void)arg;
    static upd_t s_upd;
    ca_tftp_srv_cfg_t cfg = { .local_mac = s_mac };
    while (1) {
        s_upd.slot = 0;
        bool ok = ca_tftp_receive(&cfg, upd_on_wrq, upd_on_block, &s_upd, CA_WAIT_FOREVER);
        if (s_upd.slot == 0) continue;            /* refused before choosing a slot */
        if (!ok) {
            s_upd_busy = false;
            ca_stdout_printf_n("BOOT: update FAILED after %u bytes%s - slot %s left as is",
                               s_upd.bytes, s_upd.flash_err ? " (flash write)" : "",
                               slot_name(s_upd.slot));
            continue;
        }
        s_upd_busy = false;
        if (upd_make_pending(s_upd.slot, s_upd.bytes)) {
            ca_os_sleep(300);                     /* let the line out */
            ca_os_reset();
        }
    }
    return 0U;
}

/* =============================================================================
 * Update over USB: DFU mode, dfu-util -D <image.cimg>
 * ============================================================================= */
#if EN_USB && EN_USB_DFU

#if MCU_ID == 753
#define BOOT_USB_NUM   1U        /* OTG_FS, as example_57 */
#else
#define BOOT_USB_NUM   0U        /* H563: USB_DRD_FS, the only instance */
#endif

static char s_dfu_slot;
static volatile bool s_dfu_reset;

static bool dfu_dnload(u32 off, const u8 *data, u16 len)
{
    if (off == 0U) {                              /* first block: a new file */
        if (s_upd_busy) {
            ca_stdout_put_n("BOOT: DFU refused - a TFTP update is running");
            return false;
        }
        s_upd_busy = true;
        s_dfu_slot = free_slot();
        ca_stdout_printf_n("BOOT: DFU download -> slot %s, erasing ...", slot_name(s_dfu_slot));
        if (!boot_slot_erase(s_dfu_slot)) { s_upd_busy = false; return false; }
    }
    if (off + len > BOOT_SLOT_LEN) { s_upd_busy = false; return false; }
    bool ok = ca_flash_unlock() && ca_flash_write(boot_slot_base(s_dfu_slot) + off, data, len);
    ca_flash_lock();
    if (!ok) s_upd_busy = false;
    return ok;
}

static bool dfu_manifest(u32 total)
{
    s_upd_busy = false;
    if (s_dfu_slot == 0) return false;
    const bool ok = upd_make_pending(s_dfu_slot, total);
    s_dfu_slot = 0;
    s_dfu_reset = ok;                             /* the task resets after the host is done */
    return ok;
}

static const ca_usb_dfu_cfg_t s_dfu = {
    .header        = { CA_USB_TYPE_DFU, &ca_usb_dfu_ops },
    .transfer_size = USB_DFU_XFER_MAX,
    .iface_str     = "caOS bootloader",
    .download      = true,
    .on_dnload     = dfu_dnload,
    .on_manifest   = dfu_manifest,
};

static u32 task_dfu(void *arg)
{
    (void)arg;
    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, BOOT_USB_NUM) ||
        !ca_usb_dev_mng_add(&s_dfu.header) || !ca_usb_dev_mng_start()) {
        ca_stdout_put_n("BOOT: USB DFU not available");
        return 1U;
    }
    ca_stdout_put_n("BOOT: update - dfu-util -D <image.cimg>  (USB, VID ca01)");
    while (1) {
        if (ca_usb_dfu_poll(CA_WAIT_FOREVER) && s_dfu_reset) {
            ca_os_sleep(1000);                    /* dfu-util reads the final status */
            ca_os_reset();
        }
    }
    return 0U;
}
#endif /* EN_USB && EN_USB_DFU */

#endif /* BOOT_SUPPORTED */

void ca_os_configure(void)
{
#if !BOOT_SUPPORTED
    CA_EXAMPLE_UNSUPPORTED();
#else
    char buf[DEFAULT_MSG_SIZE];
    boot_hdr_t hdr;
    const char *why = NULL;

    ca_gpio_init_port(BTN_PORT);
    ca_gpio_configure(BTN_PORT, BTN_PIN, CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0U);
    const bool held = ca_gpio_read(BTN_PORT, BTN_PIN) != 0U;  /* read before any wait */
    if (power_on())
        ca_os_delay_ms(3000U);
    boot_banner(held, buf);
    if (held) {
        s_recovery_why = "B1 held at reset";
    } else {
        char t = boot_trial(&hdr, buf);
        if (t) {
            /* the image takes the IWDG over (ca_boot_image_start) or is reset */
            if (ca_wdt_init(CA_BOOT_IWDG_MS, CA_WDT_MODE_RESET, CA_WDT_TYPE_IWDG))
                (void)ca_wdt_enable();
            boot_slot(t, &hdr, buf);          /* does not return */
        }
        char s = pick_slot(&hdr, &why, buf);
        if (s) boot_slot(s, &hdr, buf);       /* does not return */
        s_recovery_why = why;
    }
    PRINTF_N(buf, DEFAULT_MSG_SIZE, "BOOT: recovery - %s", s_recovery_why);

    /* Recovery: console, CLI, and the network for the update over TFTP */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_boot_node);
    )
    ca_flash_init();

    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&s_eth_cfg)))
        CA_SAFE_FAIL("ca_net_init");
    ca_ipv4_t zero = { .addr = 0U };
    ca_arp_service_init(s_mac, zero);
    ca_dhcp_service_init(s_mac, on_dhcp_bound, on_dhcp_expire);
    ca_tftp_service_init(s_mac);
#endif
}

void ca_os_main(void)
{
#if BOOT_SUPPORTED
    ca_arp_service_start();
    ca_dhcp_service_start();
    ca_os_job("net", CA_TH_LEVEL4, task_net, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon", CA_TH_LEVEL3, task_eth_mon, NULL, STACK_SMALL, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
    ca_os_job("update", CA_TH_LEVEL3, task_update, NULL, STACK_MEDIUM, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
#if EN_USB && EN_USB_DFU
    ca_os_job("dfu", CA_TH_LEVEL3, task_dfu, NULL, STACK_MEDIUM, CA_JOB_STD,
              CA_FAULT_RESTART_THREAD);
#endif
#endif
    ca_os_run(true);
}
