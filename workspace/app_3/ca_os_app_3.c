// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * app_3 -- L3 router USB-NCM <-> ETH between two boards (step F3a: in clear)
 *
 * Step F3 of the ETH + NCM series (ex94-ex97). Each board routes between the PC on its
 * USB-NCM and the other board on Ethernet. The stack is not modified: like
 * the relay of ex95 the whole router lives here.
 *
 *   PC (ncmA)      board 1                                 board 2             PC (ncmB)
 *   .10.1 --NCM-- ncm .10.100 | eth .50.100 ==cable== eth .50.200 | ncm .20.100 --NCM-- .20.1
 *   (all 192.168.x.0/24)
 *
 * HOW
 *   - The stack owns the ETH address (.50.x). The NCM address (.10.100 /
 *     .20.100) is the application's: it answers ARP and ping for it.
 *   - From NCM: a packet not for the board goes to Ethernet. The next hop
 *     comes from the route table (route add <net>/<len> gw <ip>, the
 *     route_service CLI); its MAC from ARP.
 *   - From ETH (the peer router): a packet for the NCM subnet goes to the PC.
 *   - Forwarding rewrites both MACs (src = this board, dst = next hop) and
 *     decrements the TTL (checksum updated incrementally, RFC 1624); nothing
 *     else in the IPv4 header changes. ca_net_send_raw(): ETH with CIC=00.
 *   - Frames reach the application through the two source-MAC filters of
 *     the stack: slot 0 = the PC on NCM (its MAC is the one this board gives
 *     in the NCM descriptor), slot 1 = the peer router, learned with ARP on
 *     the gateway of the first route. One peer router: enough for two
 *     boards on a cable.
 *
 * ROLE: board 1 or 2, negotiated on the cable at boot (see below), with
 * its addresses and its default route: any two boards of H753, H563,
 * F767. 'router' prints it all; 'route' changes the routes.
 *
 * PC procedure: each NCM interface in its own namespace with the PC address
 * and a route to the far subnet via the board (bench_net.sh, two namespaces).
 *
 * REQUIREMENTS os_config: EN_ETH, EN_NET, EN_USB, EN_USB_NCM, EN_NET_FILTER,
 *                         EN_ARP_SERVICE, EN_ROUTE_SERVICE
 */

#include "caos.h"
#include "net_proto.h"

#if MCU_ID == 753
#define USB_NUM      1U    /* OTG_FS (as ex55) */
#elif MCU_ID == 563 || MCU_ID == 767
#define USB_NUM      0U    /* H563 USB_DRD_FS / F767 OTG_FS */
#endif

#if EN_ETH && EN_NET && EN_USB && EN_USB_NCM && EN_NET_FILTER && EN_ARP_SERVICE && \
    EN_ROUTE_SERVICE && defined(USB_NUM)

#define ETH_AF         11U
#define IF_ETH         0U          /* registered first (ca_net_init) */
#define IF_USB         1U
#define FWD_DEPTH      16U
#define ICMP_ECHO_REQ  8U

/* ============================================================================
 * Addresses of the two roles
 * ============================================================================ */
typedef struct {
    ca_ipv4_t eth_ip;      /* the stack's address, on the cable           */
    ca_ipv4_t ncm_ip;      /* this board on the PC side (application's)   */
    ca_ipv4_t far_net;     /* the other board's NCM subnet                */
    ca_ipv4_t far_gw;      /* the other board's ETH address               */
    u8        last;        /* last byte of the MACs                       */
} role_t;

static const role_t ROLES[2] = {
    { .eth_ip = { .v = { 192U, 168U, 50U, 100U } }, .ncm_ip = { .v = { 192U, 168U, 10U, 100U } },
      .far_net = { .v = { 192U, 168U, 20U, 0U } },  .far_gw = { .v = { 192U, 168U, 50U, 200U } }, .last = 0x31U },
    { .eth_ip = { .v = { 192U, 168U, 50U, 200U } }, .ncm_ip = { .v = { 192U, 168U, 20U, 100U } },
      .far_net = { .v = { 192U, 168U, 10U, 0U } },  .far_gw = { .v = { 192U, 168U, 50U, 100U } }, .last = 0x32U },
};

static const ca_ipv4_t MASK24 = { .v = { 255U, 255U, 255U, 0U } };

static const role_t *volatile s_role;            /* NULL until negotiated */

/* ============================================================================
 * Role negotiation on the cable
 *
 * Any two boards (H753, H563, F767 in any pair): the role is not tied to the
 * part. Before USB and IP come up, each board broadcasts on ETH, every
 * 500 ms, a frame of EtherType ROLE_ETYPE carrying its 96-bit unique ID and
 * its role (0 = not decided):
 *   - the peer already has a role -> take the other one (a board that
 *     reboots alone gets back the role it had, the other does not move);
 *   - both undecided -> the lower unique ID is board 1.
 * After the decision the board keeps answering the peer's frames, so a
 * peer that boots later learns it. The board's ETH MAC comes from the
 * unique ID (unique, known before any role); the NCM addresses and MAC
 * come from the role.
 * ============================================================================ */
#define ROLE_ETYPE     0x88B5U        /* IEEE 802 local experimental */
#define ROLE_MAGIC     0x43413352U    /* "CA3R" */

typedef struct __attribute__((packed)) {
    u32 magic;
    u8  uid[12];
    u8  role;                          /* 0 = undecided */
} role_frame_t;

static u8            s_uid[12];
static u8            s_peer_uid[12];
static volatile bool s_peer_heard;
static volatile u8   s_peer_role;

static void role_send(void);
static u8   my_role(void) { const role_t *r = s_role; return r ? (u8)(r == &ROLES[0] ? 1U : 2U) : 0U; }
static ca_mac_t      s_mb;            /* this board, both links          */
static ca_mac_t      s_mh;            /* the PC behind NCM               */
static ca_mac_t      s_peer;          /* the peer router, learned        */
static ca_ipv4_t     s_peer_ip;
static volatile bool s_peer_ok;

static ca_net_cfg_t net_cfg = {
    .mask = { .v = { 255U, 255U, 255U, 0U } },
    .arp  = { .renew_ms = 30000U, .cache_size = 0U },
    .udp  = { .bindings = {{0}}, .count = 0U },
    .icmp = { .echo_reply = true },
};

static void on_link_change(bool up);

static ca_eth_cfg_t eth_cfg = {
    .phy_addr = 0U,
    .rmii     = true,
    .link_cb  = on_link_change,
};

/* NCM: the MAC here is the PC side one, Mh */
static ca_usb_ncm_cfg_t s_ncm = {
    .header = { CA_USB_TYPE_NCM, &ca_usb_ncm_ops },
    .mps    = 64U,
};

/* ============================================================================
 * Queue to the forwarding thread, counters
 * ============================================================================ */
typedef struct { u16 buf; u8 iface; u8 pad; } fwd_item_t;
static CAQUEUE_STATIC_DEFINE(s_fwd_q, FWD_DEPTH, sizeof(fwd_item_t));

static volatile u32 s_n2e, s_e2n, s_arp_ans, s_ping_ans;
static volatile u32 s_drop_ttl, s_drop_noroute, s_drop_q, s_tx_fail;

/* ============================================================================
 * F3b - encrypted UDP on LINK_PORT between the two routers
 *
 * A UDP packet with source or destination port LINK_PORT crosses the cable
 * only sealed by the secure pipe link (ca_sp_link_*): payload encrypted and
 * authenticated, IP addresses and ports in clear but covered by the tag
 * (AAD). Lengths and checksums are rewritten; the UDP checksum goes to 0
 * (allowed on IPv4), GCM is the integrity on the cable. Until the link is
 * up such a packet is DROPPED, never sent in clear. Any other traffic is
 * routed in clear as in F3a.
 *
 * The keys come from a password typed at every boot ('link pass <password>
 * <name>', 8..16 chars each, the same on both boards), after the crypto
 * self-tests below have passed. The handshake travels on UDP LINK_HS_PORT
 * between the two ETH addresses.
 *
 * MTU: sealing adds up to CA_SP_LINK_OVERHEAD_MAX (42) bytes, so the PCs
 * use MTU 1458 (bench_net.sh ns-router); a sealed packet over 1500 is dropped.
 * ============================================================================ */
#define LINK_PORT      5000U
#define LINK_HS_PORT   5001U
#define IP_MTU         1500U
#define UDP_HDR        8U

typedef enum { LK_SELFTEST_FAIL, LK_WAIT_PASS, LK_DERIVING, LK_HANDSHAKE, LK_UP } lk_state_t;

/* Two links, one in use: a new password is derived into the spare one
 * while the traffic keeps using the current, then they swap under s_lk_mtx
 * - which the forwarding task holds only around one seal/open. The old one
 * is wiped once nothing can reach it. No stall for the seconds of PBKDF2,
 * and no link wiped under a task that is using it. */
static ca_sp_link_t       s_links[2];
static ca_sp_link_t *volatile s_lk_cur;          /* NULL until the first password */
static ca_mutex_t         s_lk_mtx;
static volatile lk_state_t s_lk_state = LK_WAIT_PASS;
static ca_semaphore_t     s_lk_go;
static char               s_pw[CA_SP_LINK_SECRET_MAX + 1U];
static char               s_name[CA_SP_LINK_SECRET_MAX + 1U];

typedef struct { u8 b[CA_SP_LINK_HELLO_SIZE]; } hello_msg_t;
static CAQUEUE_STATIC_DEFINE(s_hello_q, 4U, sizeof(hello_msg_t));

static volatile u32 s_sealed, s_opened, s_drop_noauth, s_drop_auth, s_drop_big, s_drop_frag;
static u8 s_xbuf[IP_MTU + CA_SP_LINK_OVERHEAD_MAX + 16U];   /* fwd task only */

static bool ip_eq(ca_ipv4_t a, ca_ipv4_t b) { return a.addr == b.addr; }
static bool same_net(ca_ipv4_t a, ca_ipv4_t b) { return (a.addr & MASK24.addr) == (b.addr & MASK24.addr); }

/* ============================================================================
 * MAC filters - net task, must not block: decide, queue, return
 * true = taken by the router, false = the stack handles it
 * ============================================================================ */
static bool router_filter(u16 buf_idx, u8 iface)
{
    ca_net_buf_t        *b  = ca_net_buf_ptr(buf_idx);
    const ca_l2_frame_t *l2 = (const ca_l2_frame_t *)(b->data + b->data_off);
    const u16            et = ca_ntohs(l2->ethertype);

    if (et == ETH_P_ARP) {
        /* only the PC asking for our NCM address; the rest is the stack's */
        if (iface != IF_USB) return false;
        if (b->data_len < sizeof(ca_l2_frame_t) + sizeof(ca_arp_frame_t)) return false;
        const ca_arp_frame_t *arp = (const ca_arp_frame_t *)CA_L2_PAYLOAD(l2);
        if (ca_ntohs(arp->oper) != ARP_OP_REQUEST) return false;
        if (!ip_eq(arp->tpa, s_role->ncm_ip)) return false;
    } else if (et == ETH_P_IP) {
        if (b->data_len < sizeof(ca_l2_frame_t) + 20U) return false;
        const ca_ip4_frame_t *ip4 = (const ca_ip4_frame_t *)CA_L2_PAYLOAD(l2);
        ca_ipv4_t dst = ip4->dst;
        if (ip_eq(dst, s_role->eth_ip)) return false;          /* the stack's */
        if (iface == IF_ETH && !same_net(dst, s_role->ncm_ip)) return false;
    } else {
        return false;
    }

    fwd_item_t it = { buf_idx, iface, 0U };
    if (!ca_queue_static_trysend(&s_fwd_q.q, &it)) {
        s_drop_q++;
        ca_net_buf_free(buf_idx);
    }
    return true;
}

/* ============================================================================
 * Forwarding thread
 * ============================================================================ */

/* The board's own NCM address: ARP and ping answered from here */
static void answer_arp(u16 buf_idx)
{
    ca_net_buf_t         *b   = ca_net_buf_ptr(buf_idx);
    const ca_l2_frame_t  *l2  = (const ca_l2_frame_t *)(b->data + b->data_off);
    const ca_arp_frame_t *arp = (const ca_arp_frame_t *)CA_L2_PAYLOAD(l2);
    u16 tx = ca_net_buf_alloc();
    if (tx != NET_BUF_INVALID) {
        ca_net_buf_t *tb = ca_net_buf_ptr(tx);
        u16 n = ca_net_proto_arp_build_reply(arp, s_mb, s_role->ncm_ip,
                                             tb->data + NET_BUF_HEADROOM);
        if (n > 0U) {
            tb->data_off = NET_BUF_HEADROOM;
            tb->data_len = n;
            if (ca_net_send_on(IF_USB, tx)) s_arp_ans++; else s_tx_fail++;
        } else {
            ca_net_buf_free(tx);
        }
    }
    ca_net_buf_free(buf_idx);
}

/* true if it was a ping to our NCM address (answered, buffer freed) */
static bool answer_ping(u16 buf_idx, u8 iface)
{
    ca_net_buf_t         *b   = ca_net_buf_ptr(buf_idx);
    const ca_l2_frame_t  *l2  = (const ca_l2_frame_t *)(b->data + b->data_off);
    const ca_ip4_frame_t *ip4 = (const ca_ip4_frame_t *)CA_L2_PAYLOAD(l2);
    if (!ip_eq(ip4->dst, s_role->ncm_ip)) return false;

    if (ip4->proto == IP_PROTO_ICMP && (ip4->ver_ihl & 0x0FU) == 5U &&
        b->data_len >= sizeof(ca_l2_frame_t) + 20U + sizeof(ca_icmp_echo_t)) {
        const ca_icmp_echo_t *icmp =
            (const ca_icmp_echo_t *)((const u8 *)ip4 + 20U);
        if (icmp->type == ICMP_ECHO_REQ) {
            u16 tx = ca_net_buf_alloc();
            if (tx != NET_BUF_INVALID) {
                ca_net_buf_t *tb = ca_net_buf_ptr(tx);
                u16 n = ca_net_proto_icmp_build_echo_reply(
                            l2, ip4, icmp, s_mb, s_role->ncm_ip,
                            tb->data + NET_BUF_HEADROOM,
                            (u16)(NET_BUF_DATA_SIZE - NET_BUF_HEADROOM));
                if (n > 0U) {
                    tb->data_off = NET_BUF_HEADROOM;
                    tb->data_len = n;
                    /* back where it came from; send_on adds the checksums
                     * the builder left to the ETH offload, if out of NCM */
                    if (ca_net_send_on(iface, tx)) s_ping_ans++; else s_tx_fail++;
                } else {
                    ca_net_buf_free(tx);
                }
            }
        }
    }
    ca_net_buf_free(buf_idx);           /* any other protocol to us: dropped */
    return true;
}

/* TTL - 1 with the header checksum updated in place (RFC 1624): the TTL is
 * the high byte of its 16-bit word, so the sum grows by 0x0100. */
static bool ttl_dec(ca_ip4_frame_t *ip4)
{
    if (ip4->ttl <= 1U) return false;
    ip4->ttl--;
    u32 sum = (u32)ca_ntohs(ip4->cksum) + 0x0100U;
    sum = (sum & 0xFFFFU) + (sum >> 16);
    ip4->cksum = ca_htons((u16)sum);
    return true;
}

/* UDP on LINK_PORT: seal toward the cable, open toward the PC. Returns
 * false if the packet must be dropped (counted). Anything else: true,
 * untouched. Runs in the fwd task (s_xbuf is its scratch). */
static bool protect(ca_net_buf_t *b, ca_ip4_frame_t *ip4, bool to_cable)
{
    if (ip4->proto != IP_PROTO_UDP) return true;
    /* A fragment past the first has no ports: whether it belongs to the
     * protected port cannot be told, so no UDP fragment crosses at all. */
    if (ca_ntohs(ip4->flags_frag) & (IP_FLAG_MF | IP_FRAG_OFF_MASK)) {
        s_drop_frag++;
        return false;
    }
    u16 ihl = CA_IP4_IHL(ip4);
    u16 tot = ca_ntohs(ip4->total_len);
    if (ihl < 20U || tot < ihl + UDP_HDR) return true;
    ca_udp_frame_t *udp = (ca_udp_frame_t *)((u8 *)ip4 + ihl);
    if (ca_ntohs(udp->src_port) != LINK_PORT && ca_ntohs(udp->dst_port) != LINK_PORT)
        return true;

    /* AAD: who talks to whom - the addresses and ports, which stay in clear */
    u8 aad[12];
    ca_memcpy(aad, &ip4->src, 4U);
    ca_memcpy(aad + 4U, &ip4->dst, 4U);
    ca_memcpy(aad + 8U, &udp->src_port, 2U);
    ca_memcpy(aad + 10U, &udp->dst_port, 2U);

    u8  *payload = (u8 *)udp + UDP_HDR;
    u32  plen    = (u32)(tot - ihl - UDP_HDR);
    u32  nlen    = 0U;
    i32  rc;
    ca_mutex_lock(&s_lk_mtx);
    ca_sp_link_t *l = s_lk_cur;
    if (l == NULL || !ca_sp_link_is_up(l)) {
        ca_mutex_unlock(&s_lk_mtx);
        s_drop_noauth++;
        return false;
    }
    if (to_cable) {
        rc = (plen == 0U) ? CA_SECURE_PIPE_ERR :
             ca_sp_link_seal(l, aad, sizeof(aad), payload, plen, s_xbuf, sizeof(s_xbuf), &nlen);
    } else {
        rc = ca_sp_link_open(l, aad, sizeof(aad), payload, plen, s_xbuf, sizeof(s_xbuf), &nlen);
    }
    ca_mutex_unlock(&s_lk_mtx);
    if (rc != CA_SECURE_PIPE_OK) {
        if (to_cable) s_drop_noauth++; else s_drop_auth++;
        return false;
    }
    if ((u32)ihl + UDP_HDR + nlen > IP_MTU) {
        ca_memzero(s_xbuf, nlen);
        s_drop_big++;
        return false;
    }

    ca_memcpy(payload, s_xbuf, nlen);
    ca_memzero(s_xbuf, nlen);
    tot = (u16)(ihl + UDP_HDR + nlen);
    udp->length   = ca_htons((u16)(UDP_HDR + nlen));
    udp->checksum = 0U;                         /* none on IPv4 (RFC 768) */
    ip4->total_len = ca_htons(tot);
    ip4->cksum = 0U;
    ip4->cksum = ca_htons(ca_net_cksum((const u8 *)ip4, ihl));
    b->data_len = (u16)(sizeof(ca_l2_frame_t) + tot);
    if (to_cable) s_sealed++; else s_opened++;
    return true;
}

static void forward(u16 buf_idx, u8 iface)
{
    ca_net_buf_t   *b   = ca_net_buf_ptr(buf_idx);
    ca_l2_frame_t  *l2  = (ca_l2_frame_t *)(b->data + b->data_off);
    ca_ip4_frame_t *ip4 = (ca_ip4_frame_t *)CA_L2_PAYLOAD(l2);
    ca_ipv4_t       dst = ip4->dst;
    u8              out_if;

    if (iface == IF_USB) {                              /* PC -> cable */
        if (same_net(dst, s_role->ncm_ip)) {            /* back to its own net */
            s_drop_noroute++; ca_net_buf_free(buf_idx); return;
        }
        ca_ipv4_t nh = ca_route_lookup(dst);
        ca_mac_t  mac;
        if (!same_net(nh, s_role->eth_ip) ||
            (!ca_arp_find(nh, &mac) && !ca_arp_resolve(nh, &mac, 200U))) {
            s_drop_noroute++; ca_net_buf_free(buf_idx); return;
        }
        l2->dst = mac;
        out_if  = IF_ETH;
    } else {                                            /* cable -> PC */
        l2->dst = s_mh;
        out_if  = IF_USB;
    }

    if (!ttl_dec(ip4)) { s_drop_ttl++; ca_net_buf_free(buf_idx); return; }
    if (!protect(b, ip4, out_if == IF_ETH)) { ca_net_buf_free(buf_idx); return; }
    l2->src = s_mb;

    /* The iface owns the buffer from here, whatever it returns */
    if (ca_net_send_raw(out_if, buf_idx)) {
        if (out_if == IF_ETH) s_n2e++; else s_e2n++;
    } else {
        s_tx_fail++;
    }
}

/* The peer router: gateway of the first route, MAC by ARP. Its frames reach
 * the router through filter slot 1. Rechecked every second: a route added
 * or changed from the CLI is picked up here. */
static void refresh_peer(void)
{
    ca_route_entry_t e;
    for (u8 i = 0U; i < NET_ROUTE_MAX; i++) {
        if (!ca_route_get(i, &e)) continue;
        ca_mac_t mac;
        if (!ca_arp_find(e.gw, &mac) && !ca_arp_resolve(e.gw, &mac, 500U)) return;
        if (!s_peer_ok || !ip_eq(e.gw, s_peer_ip) || ca_memcmp(mac.v, s_peer.v, 6U) != 0) {
            s_peer    = mac;
            s_peer_ip = e.gw;
            s_peer_ok = true;
            ca_net_filter_mac_set(1U, &s_peer, router_filter);
            ca_stdout_printf_n("ROUTER peer %u.%u.%u.%u %02X:%02X:%02X:%02X:%02X:%02X",
                e.gw.v[0], e.gw.v[1], e.gw.v[2], e.gw.v[3],
                mac.v[0], mac.v[1], mac.v[2], mac.v[3], mac.v[4], mac.v[5]);
        }
        return;                                         /* first route only */
    }
}

/* ============================================================================
 * F3b - boot self-tests, link task, handshake on UDP LINK_HS_PORT
 * ============================================================================ */

/* Known answers of every primitive the link rests on. Copied here on
 * purpose: in caos/crypto the self-tests live under the _TEST flags, while
 * this router must run them on every boot, before it accepts a password. */
static u32 nib(char c)
{
    u32 v = (u32)(u8)c;
    return (v <= (u32)'9') ? v - (u32)'0' : v - (u32)'a' + 10U;
}
static bool hex_eq(const u8 *got, const char *hex, u32 n)
{
    for (u32 i = 0U; i < n; i++)
        if ((u32)got[i] != ((nib(hex[2U * i]) << 4) | nib(hex[2U * i + 1U]))) return false;
    return true;
}

static bool selftest(void)
{
    static ca_aes_ctx_t ctx;            /* static: not on the task stack */
    u8   key[32], iv[16], buf[64], tag[16];
    bool ok = true, r;

    /* AES-256-GCM, NIST test case 14 (key, IV, 16 bytes all zero) */
    ca_memset(key, 0U, sizeof(key));
    ca_memset(iv, 0U, sizeof(iv));
    iv[15] = 2U;                         /* CRYP layout: nonce | counter 2 */
    ca_memset(buf, 0U, 16U);
    r = ca_aes_ctx_setup(&ctx, CA_AES_MODE_GCM, CA_AES_KEY_256, key, iv) &&
        ca_aes_gcm_encrypt(&ctx, NULL, 0U, buf, buf, 16U, tag) &&
        hex_eq(buf, "cea7403d4d606b6e074ec5d3baf39d18", 16U) &&
        hex_eq(tag, "d0d1c8a799996bf0265b98b5d48ab919", 16U);
    ca_stdout_printf_n("  aes-256-gcm nist #14    %s", r ? "PASS" : "FAIL"); ok &= r;

    /* same, with AAD - the path the link uses (Python cryptography) */
    ca_memset(buf, 0U, 16U);
    r = ca_aes_ctx_setup(&ctx, CA_AES_MODE_GCM, CA_AES_KEY_256, key, iv) &&
        ca_aes_gcm_encrypt(&ctx, (const u8 *)"caos-app3-aad", 13U, buf, buf, 16U, tag) &&
        hex_eq(tag, "4cf47dcac4740108a81656a6c28fc2f5", 16U);
    ca_stdout_printf_n("  aes-256-gcm aad         %s", r ? "PASS" : "FAIL"); ok &= r;
    ca_memzero(&ctx, sizeof(ctx));

    /* HMAC-SHA256, RFC 4231 #1 */
    ca_memset(key, 0x0BU, 20U);
    ca_hmac_sha256(key, 20U, (const u8 *)"Hi There", 8U, buf);
    r = hex_eq(buf, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32U);
    ca_stdout_printf_n("  hmac-sha256 rfc4231 #1  %s", r ? "PASS" : "FAIL"); ok &= r;

    /* HKDF-SHA256, RFC 5869 #3 (no salt, no info) */
    ca_memset(key, 0x0BU, 22U);
    r = ca_hkdf_sha256(NULL, 0U, key, 22U, NULL, 0U, buf, 42U) &&
        hex_eq(buf, "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
                    "9d201395faa4b61a96c8", 42U);
    ca_stdout_printf_n("  hkdf-sha256 rfc5869 #3  %s", r ? "PASS" : "FAIL"); ok &= r;

    /* PBKDF2-HMAC-SHA256, RFC 7914 §11, c = 1 */
    r = ca_pbkdf2_sha256((const u8 *)"passwd", 6U, (const u8 *)"salt", 4U, 1U, buf, 64U) &&
        hex_eq(buf, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
                    "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783", 64U);
    ca_stdout_printf_n("  pbkdf2-sha256 rfc7914   %s", r ? "PASS" : "FAIL"); ok &= r;

    /* RNG: two draws must differ (a stuck generator would give a fixed nonce) */
    u8 a[16], c[16];
    r = ca_rng_fill(a, sizeof(a)) && ca_rng_fill(c, sizeof(c)) &&
        ca_memcmp(a, c, sizeof(a)) != 0;
    ca_stdout_printf_n("  rng two draws differ    %s", r ? "PASS" : "FAIL"); ok &= r;

    ca_memzero(key, sizeof(key));
    ca_memzero(buf, sizeof(buf));
    return ok;
}

/* HELLO to the peer router: UDP LINK_HS_PORT, built by hand and sent raw */
static void send_hello(bool reply)
{
    if (!s_peer_ok) return;
    u8  h[CA_SP_LINK_HELLO_SIZE];
    u32 hl = 0U;
    if (s_lk_cur == NULL ||
        ca_sp_link_hello(s_lk_cur, reply, h, sizeof(h), &hl) != CA_SECURE_PIPE_OK) return;

    u16 tx = ca_net_buf_alloc();
    if (tx == NET_BUF_INVALID) return;
    ca_net_buf_t   *tb  = ca_net_buf_ptr(tx);
    u8             *f   = tb->data + NET_BUF_HEADROOM;
    ca_l2_frame_t  *l2  = (ca_l2_frame_t *)f;
    ca_ip4_frame_t *ip4 = (ca_ip4_frame_t *)CA_L2_PAYLOAD(l2);
    ca_udp_frame_t *udp = (ca_udp_frame_t *)((u8 *)ip4 + 20U);
    static u16 s_ip_id;
    u16 tot = (u16)(20U + UDP_HDR + hl);

    l2->dst = s_peer;
    l2->src = s_mb;
    l2->ethertype = ca_htons(ETH_P_IP);
    ca_memset(ip4, 0U, 20U);
    ip4->ver_ihl   = 0x45U;
    ip4->total_len = ca_htons(tot);
    ip4->id        = ca_htons(++s_ip_id);
    ip4->ttl       = 64U;
    ip4->proto     = IP_PROTO_UDP;
    ip4->src       = s_role->eth_ip;
    ip4->dst       = s_peer_ip;
    ip4->cksum     = ca_htons(ca_net_cksum((const u8 *)ip4, 20U));
    udp->src_port  = ca_htons(LINK_HS_PORT);
    udp->dst_port  = ca_htons(LINK_HS_PORT);
    udp->length    = ca_htons((u16)(UDP_HDR + hl));
    udp->checksum  = 0U;
    ca_memcpy((u8 *)udp + UDP_HDR, h, hl);

    tb->data_off = NET_BUF_HEADROOM;
    tb->data_len = (u16)(sizeof(ca_l2_frame_t) + tot);
    if (!ca_net_send_raw(IF_ETH, tx)) s_tx_fail++;
}

/* net task: copy the HELLO out and return, the link task does the crypto */
static void hello_rx(u16 buf_idx)
{
    ca_net_buf_t *b  = ca_net_buf_ptr(buf_idx);
    const u8     *fr = b->data + b->data_off;
    u16           l4 = b->meta.rx.l4_off;
    if (b->data_len >= l4 + UDP_HDR + CA_SP_LINK_HELLO_SIZE) {
        hello_msg_t m;
        ca_memcpy(m.b, fr + l4 + UDP_HDR, CA_SP_LINK_HELLO_SIZE);
        (void)ca_queue_static_trysend(&s_hello_q.q, &m);
    }
    ca_net_buf_free(buf_idx);
}

static u32 task_link(void *arg)
{
    (void)arg;
    ca_os_sleep(800);
    ca_stdout_put_n("=== APP3 link: crypto self-tests ===");
    if (!selftest()) {
        s_lk_state = LK_SELFTEST_FAIL;
        ca_stdout_put_n("LINK SELFTEST FAIL -- encrypted link disabled, port 5000 dropped");
        while (1) ca_os_sleep(60000);
    }
    ca_stdout_put_n("LINK SELFTEST PASS -- 'link pass <password> <name>' to start");

    while (1) {
        ca_sem_wait(&s_lk_go);                          /* a password came in */
        s_lk_state = LK_DERIVING;
        /* derive into the spare link; the current one keeps working */
        ca_sp_link_t *spare = (s_lk_cur == &s_links[0]) ? &s_links[1] : &s_links[0];
        u32 t0 = ca_os_tick_ms();
        while (s_role == NULL) ca_os_sleep(200);          /* role decides K12/K21 */
        i32 rc = ca_sp_link_init(spare, my_role(), s_pw, s_name);
        ca_memzero(s_pw, sizeof(s_pw));
        ca_memzero(s_name, sizeof(s_name));
        if (rc != CA_SECURE_PIPE_OK) {
            s_lk_state = (s_lk_cur != NULL && ca_sp_link_is_up(s_lk_cur)) ? LK_UP : LK_WAIT_PASS;
            ca_stdout_put_n("LINK keys FAILED");
            continue;
        }
        ca_mutex_lock(&s_lk_mtx);                       /* no seal/open in flight */
        ca_sp_link_t *old = s_lk_cur;
        s_lk_cur = spare;
        ca_mutex_unlock(&s_lk_mtx);
        if (old != NULL) ca_sp_link_deinit(old);         /* unreachable now: wipe */
        ca_stdout_printf_n("LINK keys derived in %u ms", ca_os_tick_ms() - t0);
        s_lk_state = LK_HANDSHAKE;
        {   /* HELLOs that arrived before the keys existed are stale */
            hello_msg_t old;
            while (ca_queue_static_tryreceive(&s_hello_q.q, &old)) { }
        }

        /* until a new password: answer HELLOs, retry ours while down, rekey */
        u32 last = 0U;
        while (ca_sem_trywait(&s_lk_go) == false) {
            hello_msg_t m;
            if (ca_queue_static_receive(&s_hello_q.q, &m, 200U)) {
                rc = ca_sp_link_accept(s_lk_cur, m.b, sizeof(m.b));
                if (rc == CA_SECURE_PIPE_REPLY) send_hello(true);
            }
            bool up = ca_sp_link_is_up(s_lk_cur);
            if (up && s_lk_state != LK_UP) ca_stdout_put_n("LINK UP");
            if (!up && s_lk_state == LK_UP) ca_stdout_put_n("LINK DOWN");
            s_lk_state = up ? LK_UP : LK_HANDSHAKE;
            /* 3 s between our HELLOs: a fresh one replaces our nonce, and a
             * reply to the previous one would pair with the wrong nonce */
            if ((!up || ca_sp_link_need_rekey(s_lk_cur)) && ca_os_tick_ms() - last >= 3000U) {
                last = ca_os_tick_ms();
                send_hello(false);
            }
        }
        ca_sem_post(&s_lk_go);                          /* the trywait took it */
    }
    return 0;
}

static u32 task_fwd(void *arg)
{
    (void)arg;
    ca_os_sleep(1500);                     /* after the net task is up */
    u32 last = 0U;
    while (1) {
        fwd_item_t it;
        if (ca_queue_static_receive(&s_fwd_q.q, &it, 1000U)) {
            ca_net_buf_t        *b  = ca_net_buf_ptr(it.buf);
            const ca_l2_frame_t *l2 = (const ca_l2_frame_t *)(b->data + b->data_off);
            if (ca_ntohs(l2->ethertype) == ETH_P_ARP) answer_arp(it.buf);
            else if (!answer_ping(it.buf, it.iface)) forward(it.buf, it.iface);
        }
        if (ca_os_tick_ms() - last >= 1000U) {
            last = ca_os_tick_ms();
            refresh_peer();
        }
    }
    return 0;
}

/* ============================================================================
 * Role negotiation: frames (see the top of the file)
 * ============================================================================ */
static void role_send(void)
{
    u16 tx = ca_net_buf_alloc();
    if (tx == NET_BUF_INVALID) return;
    ca_net_buf_t  *tb = ca_net_buf_ptr(tx);
    ca_l2_frame_t *l2 = (ca_l2_frame_t *)(tb->data + NET_BUF_HEADROOM);
    role_frame_t  *rf = (role_frame_t *)CA_L2_PAYLOAD(l2);
    ca_memset(l2->dst.v, 0xFFU, 6U);
    l2->src = s_mb;
    l2->ethertype = ca_htons(ROLE_ETYPE);
    rf->magic = ca_htonl(ROLE_MAGIC);
    ca_memcpy(rf->uid, s_uid, sizeof(s_uid));
    rf->role = my_role();
    tb->data_off = NET_BUF_HEADROOM;
    tb->data_len = 60U;                               /* Ethernet minimum */
    ca_memset((u8 *)rf + sizeof(*rf), 0U,
              60U - sizeof(ca_l2_frame_t) - sizeof(*rf));
    (void)ca_net_send_raw(IF_ETH, tx);
}

/* EtherType filter, net task: owns buf_idx */
static void role_rx(u16 buf_idx)
{
    ca_net_buf_t        *b  = ca_net_buf_ptr(buf_idx);
    const ca_l2_frame_t *l2 = (const ca_l2_frame_t *)(b->data + b->data_off);
    const role_frame_t  *rf = (const role_frame_t *)CA_L2_PAYLOAD(l2);
    if (b->data_len >= sizeof(ca_l2_frame_t) + sizeof(*rf) &&
        ca_ntohl(rf->magic) == ROLE_MAGIC && rf->role <= 2U &&
        ca_memcmp(rf->uid, s_uid, sizeof(s_uid)) != 0) {
        ca_memcpy(s_peer_uid, rf->uid, sizeof(s_peer_uid));
        s_peer_role  = rf->role;
        s_peer_heard = true;
        /* decided already, and the peer is not: answer, it is waiting */
        if (s_role != NULL && rf->role == 0U) role_send();
    }
    ca_net_buf_free(buf_idx);
}

/* ============================================================================
 * Net task
 * ============================================================================ */
static bool s_net_up;

static u32 task_net(void *arg)
{
    (void)arg;
    if (s_net_up) {
        ca_stdout_printf_n("Net task restarted -- init kept");
        goto rx_loop;
    }
    ca_os_sleep(300);

    /* Role first: the NCM addresses and MAC depend on it, and USB must not
     * enumerate before. The rx loop runs here meanwhile, or the peer's
     * frames would never be processed. */
    ca_stdout_printf_n("APP3: negotiating the role on the cable (uid %02X%02X%02X%02X...)",
                       s_uid[0], s_uid[1], s_uid[2], s_uid[3]);
    ca_net_filter_set(ROLE_ETYPE, role_rx);
    u32 last_tx = 0U, last_msg = ca_os_tick_ms();
    while (s_role == NULL) {
        if (ca_os_tick_ms() - last_tx >= 500U) { last_tx = ca_os_tick_ms(); role_send(); }
        if (ca_sem_timedwait(&ca_net_rx_sem, 100U)) ca_net_process();
        if (s_peer_heard && s_role == NULL) {
            u8 r = (s_peer_role != 0U) ? (u8)(3U - s_peer_role)
                 : (ca_memcmp(s_uid, s_peer_uid, sizeof(s_uid)) < 0 ? 1U : 2U);
            s_role = &ROLES[r - 1U];
        }
        if (s_role == NULL && ca_os_tick_ms() - last_msg >= 10000U) {
            last_msg = ca_os_tick_ms();
            ca_stdout_put_n("APP3: no peer on the cable yet (ETH link? other board running app_3?)");
        }
    }
    role_send();                       /* tell the peer at once */
    const u8 mh[6] = { 0x02U, 0xCAU, 0x05U, 0x00U, 0x03U, s_role->last };
    ca_memcpy(s_mh.v, mh, 6U);
    ca_memcpy(s_ncm.mac, mh, 6U);
    net_cfg.ip = s_role->eth_ip;
    ca_arp_service_update_local_ip(s_role->eth_ip);

    ca_stdout_printf_n("\n\n=== APP3: L3 router NCM <-> ETH, board %u ===", (u32)my_role());
    ca_stdout_printf_n("ETH  %u.%u.%u.%u  NCM %u.%u.%u.%u",
        s_role->eth_ip.v[0], s_role->eth_ip.v[1], s_role->eth_ip.v[2], s_role->eth_ip.v[3],
        s_role->ncm_ip.v[0], s_role->ncm_ip.v[1], s_role->ncm_ip.v[2], s_role->ncm_ip.v[3]);

    if (!ca_usb_dev_mng_init(CA_USB_SPEED_FS, USB_NUM)) {
        ca_stdout_printf_n("[USB] init FAILED"); return 1U;
    }
    if (!ca_usb_dev_mng_add(&s_ncm.header)) {
        ca_stdout_printf_n("[USB] add NCM FAILED"); return 1U;
    }
    ca_usb_dev_mng_start();
    if (!ca_net_register_iface(ca_ncm_iface_get(&s_ncm))) {
        ca_stdout_printf_n("[NET] register usb0 FAILED"); return 1U;
    }

    ca_net_proto_init(&net_cfg);
    /* default route of the role; 'route' changes it */
    (void)ca_route_add(s_role->far_net, 24U, s_role->far_gw);

    ca_net_filter_mac_set(0U, &s_mh, router_filter);   /* the PC; slot 1 = peer */
    (void)ca_net_proto_bind_udp(LINK_HS_PORT, hello_rx); /* F3b handshake */

    for (u8 i = 0U; i < ca_net_iface_count(); i++) {
        const ca_net_iface_t *f = ca_net_iface_at(i);
        ca_stdout_printf_n("iface %u %s csum=%s", (u32)f->id, f->name,
                           f->csum_offload ? "hw" : "sw");
    }

    ca_os_sleep(500);
    ca_eth_link_poll();
    ca_arp_send_garp();
    ca_os_sleep(50);
    ca_stdout_printf_n("Router ready");
    s_net_up = true;

rx_loop:
    while (1) {
        ca_sem_wait(&ca_net_rx_sem);
        ca_net_process();
    }
    return 0;
}

/* ============================================================================
 * CLI: router
 * ============================================================================ */
#if ENABLE_CLI
static bool router_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("router    : addresses, peer and counters of the L3 router");
        return false;
    }
    if (ca_strncmp(p, "router", 6) != 0) return false;
    const role_t *r = s_role;
    if (r == NULL) {
        CLI_PRINTF_N("router board 0  (negotiating the role, peer %s)",
                     s_peer_heard ? "heard" : "not heard");
        return true;
    }
    CLI_PRINTF_N("router board %u  eth %u.%u.%u.%u  ncm %u.%u.%u.%u",
                 (u32)my_role(),
                 r->eth_ip.v[0], r->eth_ip.v[1], r->eth_ip.v[2], r->eth_ip.v[3],
                 r->ncm_ip.v[0], r->ncm_ip.v[1], r->ncm_ip.v[2], r->ncm_ip.v[3]);
    CLI_PRINTF_N("router peer %u.%u.%u.%u %s",
                 s_peer_ip.v[0], s_peer_ip.v[1], s_peer_ip.v[2], s_peer_ip.v[3],
                 s_peer_ok ? "known" : "unknown");
    CLI_PRINTF_N("router n2e=%u e2n=%u arp_ans=%u ping_ans=%u",
                 s_n2e, s_e2n, s_arp_ans, s_ping_ans);
    CLI_PRINTF_N("router drop_ttl=%u drop_noroute=%u drop_q=%u tx_fail=%u",
                 s_drop_ttl, s_drop_noroute, s_drop_q, s_tx_fail);
    return true;
}

/* link pass <password> <name> | link status
 * The password is copied, then wiped from the command buffer and from the
 * CLI history: the arrow keys must not bring it back. */
static const char *const LK_NAMES[] = { "SELFTEST FAIL", "waiting password",
                                        "deriving keys", "handshake", "UP" };

static u32 word_len(const char *p)
{
    u32 n = 0U;
    while (p[n] != '\0' && p[n] != ' ') n++;
    return n;
}

static bool link_cli_handler(c_str *pcmd)
{
    char *p = (char *)ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("link      : pass <password> <name> (8..16 chars) | status");
        return false;
    }
    if (ca_strncmp(p, "link", 4) != 0) return false;
    p = (char *)ca_skip_sp(p + 4);

    if (ca_strncmp(p, "pass", 4) == 0) {
        char *pw = (char *)ca_skip_sp(p + 4);
        u32   pl = word_len(pw);
        char *nm = (char *)ca_skip_sp(pw + pl);
        u32   nl = word_len(nm);
        bool  ok = s_lk_state != LK_SELFTEST_FAIL && s_lk_state != LK_DERIVING &&
                   pl >= CA_SP_LINK_SECRET_MIN && pl <= CA_SP_LINK_SECRET_MAX &&
                   nl >= CA_SP_LINK_SECRET_MIN && nl <= CA_SP_LINK_SECRET_MAX;
        if (ok) {
            ca_memcpy(s_pw, pw, pl);   s_pw[pl] = '\0';
            ca_memcpy(s_name, nm, nl); s_name[nl] = '\0';
        }
        ca_memzero((void *)(*pcmd)->data, ca_strlen((*pcmd)->data));   /* the CLI copy: writable */
        ca_cli_hist_forget("link pass");
        if (!ok) {
            CLI_PUT_N("link pass: password and name 8..16 chars, after the self-tests");
            return true;
        }
        ca_sem_post(&s_lk_go);
        CLI_PUT_N("link: deriving keys (seconds)...");
        return true;
    }
    if (ca_strncmp(p, "status", 6) == 0) {
        static const ca_sp_link_stats_t none;
        const ca_sp_link_stats_t *st = s_lk_cur ? &s_lk_cur->st : &none;
        CLI_PRINTF_N("link %s  port %u  hello ok=%u bad=%u sessions=%u", LK_NAMES[s_lk_state],
                     (u32)LINK_PORT, st->hello_ok, st->hello_bad, st->sessions);
        CLI_PRINTF_N("link sealed=%u opened=%u tag_fail=%u replay=%u", s_sealed, s_opened,
                     st->tag_fail, st->replay);
        CLI_PRINTF_N("link drop_noauth=%u drop_auth=%u drop_big=%u drop_frag=%u",
                     s_drop_noauth, s_drop_auth, s_drop_big, s_drop_frag);
        return true;
    }
    CLI_PUT_N("link: pass <password> <name> | status");
    return true;
}
static ca_cli_node_t s_link_cli = { link_cli_handler, NULL };
static ca_cli_node_t s_router_cli = { router_cli_handler, NULL };
#endif

/* ============================================================================
 * Link callback / monitor / heartbeat
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
 * GPIO ETH RMII (AF11) - same wiring as ex33
 * ============================================================================ */
static void eth_gpio_init(void)
{
    static const struct { u8 port; u8 pin; } pins[] = {
        { CA_GPIO_PORT_A,  1U }, { CA_GPIO_PORT_A,  2U }, { CA_GPIO_PORT_A,  7U },
        { CA_GPIO_PORT_C,  1U }, { CA_GPIO_PORT_C,  4U }, { CA_GPIO_PORT_C,  5U },
        { CA_GPIO_PORT_G, 11U }, { CA_GPIO_PORT_G, 13U },
        /* RMII_TXD1: NUCLEO-H563ZI (UM3115) uses PB15 (JP6 ON); the others PB13 */
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
        ca_cli_register(&s_router_cli);
        ca_cli_register(&s_link_cli);
        ca_cli_set_secret_prefix("link pass ");   /* typed as '*', never in the history */
    )

    /* Board identity from the 96-bit unique ID: the ETH MAC is
     * 02:00 + the ID folded to 4 bytes (locally administered, unique per
     * part). The role - and with it the IPs and the NCM MAC - comes later,
     * negotiated on the cable (task_net). On M33 (H563) UID_BASE is inside
     * the non-cacheable RO MPU region, so the read is safe. */
    for (u32 i = 0U; i < 3U; i++) {
        u32 w = ((const volatile u32 *)UID_BASE)[i];
        s_uid[4U * i]      = (u8)(w >> 24);
        s_uid[4U * i + 1U] = (u8)(w >> 16);
        s_uid[4U * i + 2U] = (u8)(w >> 8);
        s_uid[4U * i + 3U] = (u8)w;
    }
    const u8 mb[6] = { 0x02U, 0x00U,
                       (u8)(s_uid[0] ^ s_uid[4] ^ s_uid[8]),  (u8)(s_uid[1] ^ s_uid[5] ^ s_uid[9]),
                       (u8)(s_uid[2] ^ s_uid[6] ^ s_uid[10]), (u8)(s_uid[3] ^ s_uid[7] ^ s_uid[11]) };
    ca_memcpy(s_mb.v, mb, 6U);
    eth_cfg.mac = s_mb;
    net_cfg.mac = s_mb;

    ca_led_init();
    ca_led_off(1U);
    ca_led_off(3U);

    if (!ca_aes_init()) CA_SAFE_FAIL("ca_aes_init");
    if (!ca_rng_init())  CA_SAFE_FAIL("ca_rng_init");
    /* 4 slots per link, two links while a new password replaces the old */
    if (ca_keys_store_init(8) != 0) CA_SAFE_FAIL("ca_keys_store_init");
    ca_sem_init(&s_lk_go, "lk_go", 0U, 1U);
    ca_mutex_init(&s_lk_mtx);

    /* iface 0: ETH. usb0 joins from the net task, once USB is up. */
    eth_gpio_init();
    if (!ca_net_init(ca_eth_iface_get(&eth_cfg))) {
        CA_SAFE_FAIL("ca_net_init");
    }
    /* the IP comes with the role: ca_arp_service_update_local_ip() then */
    static const ca_ipv4_t no_ip = { .v = { 0U, 0U, 0U, 0U } };
    ca_arp_service_init(s_mb, no_ip);
}

void ca_os_main(void)
{
    /* The task that sends the ARP requests: without it ca_arp_resolve()
     * only ever finds what the cache learned from incoming traffic - two
     * routers waiting for each other never resolve. */
    ca_arp_service_start();
    ca_os_job("net",       CA_TH_LEVEL4, task_net,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("fwd",       CA_TH_LEVEL4, task_fwd,       NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("link",      CA_TH_LEVEL3, task_link,      NULL,
              STACK_LARGE, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("eth_mon",   CA_TH_LEVEL3, task_eth_mon,   NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("heartbeat", CA_TH_LEVEL4, task_heartbeat, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(ma3a, "=== app_3 ===");
PRIVATE_STRING(ma3b, "needs ETH + USB device (EN_ETH / EN_NET / EN_USB / EN_USB_NCM / EN_NET_FILTER / EN_ROUTE_SERVICE)");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_ma3a);
    PUT_N(&priv_ma3b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
