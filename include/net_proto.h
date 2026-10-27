// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 * @file  net_proto.h
 * @brief caOS network protocol — struct wire-format packed 1:1 RFC, L2/L3/L4.
 *
 *        All structs map exactly to the wire format.
 *        Navigation by casting the payload to the next level:
 *
 *          raw frame (u8 *, len)
 *            └─ ca_net_proto_l2()        → ca_l2_frame_t *
 *                 │  ethertype==ETH_P_VLAN → ca_l2_vlan_frame_t *
 *                 ├─ ETH_P_ARP  → ca_net_proto_arp()       → ca_arp_frame_t *
 *                 └─ ETH_P_IP   → ca_net_proto_ip4()       → ca_ip4_frame_t *
 *                                    ├─ IP_PROTO_UDP  → ca_net_proto_udp()        → ca_udp_frame_t *
 *                                    └─ IP_PROTO_ICMP → ca_net_proto_icmp_echo()  → ca_icmp_echo_t *
 *
 *        Next level payload — navigation macros:
 *          CA_L2_PAYLOAD(l2)        → bytes after Ethernet header (14 bytes)
 *          CA_L2_VLAN_PAYLOAD(l2v)  → bytes after 802.1Q header   (18 bytes)
 *          CA_IP4_PAYLOAD(ip4)      → bytes after IPv4 header (IHL-aware)
 *          CA_UDP_PAYLOAD(udp)      → bytes after UDP header (8 bytes)
 *
 *        All multi-byte fields: big-endian (network order).
 *        Use ca_ntohs() / ca_ntohl() to read them as host integers.
 */

#ifndef NET_PROTO_H_
#define NET_PROTO_H_

#ifdef CAOS_PC_TEST
#include "caos_pc.h"
#else
#include "common.h"
#include "os_config.h"
#endif

#include "net_cfg.h"

/* ----------------------------------------------------------------------- */
/* EtherType / IP proto constants                                           */
/* ----------------------------------------------------------------------- */
#ifndef ETH_P_IP
#define ETH_P_IP    0x0800U
#define ETH_P_ARP   0x0806U
#define ETH_P_IPV6  0x86DDU
#define ETH_P_VLAN  0x8100U
#endif

#define IP_PROTO_ICMP   1U
#define IP_PROTO_TCP    6U
#define IP_PROTO_UDP   17U

/* L2 payload assumed when the interface does not declare one (RFC 894). */
#define IP_TX_MTU_DEFAULT 1500U

/* IP flags, in host order — the field is stored big-endian.
 * MF marks "more fragments follow"; DF forbids fragmenting at all. */
#define IP_FLAG_DF     0x4000U
#define IP_FLAG_MF     0x2000U
#define IP_FRAG_OFF_MASK 0x1FFFU

#define ARP_HTYPE_ETH   1U
#define ARP_PTYPE_IP    0x0800U
#define ARP_OP_REQUEST  1U
#define ARP_OP_REPLY    2U

/* ----------------------------------------------------------------------- */
/* Wire-format structs — packed, 1:1 with RFC                               */
/* All multi-byte fields: big-endian. Use ca_ntohs/ca_ntohl.         */
/* ----------------------------------------------------------------------- */

/** Ethernet II header — 14 bytes (IEEE 802.3) */
typedef struct {
    ca_mac_t  dst;         /* destination MAC */
    ca_mac_t  src;         /* source MAC */
    u16    ethertype;   /* EtherType big-endian: ETH_P_IP / ETH_P_ARP / ETH_P_VLAN */
} __attribute__((packed)) ca_l2_frame_t;

/** Ethernet 802.1Q VLAN header — 18 bytes */
typedef struct {
    ca_mac_t  dst;         /* destination MAC */
    ca_mac_t  src;         /* source MAC */
    u16    tpid;        /* 0x8100 big-endian */
    u16    tci;         /* PCP[15:13] | DEI[12] | VID[11:0] big-endian */
    u16    ethertype;   /* inner EtherType big-endian */
} __attribute__((packed)) ca_l2_vlan_frame_t;

/** ARP for IPv4/Ethernet — 28 bytes (RFC 826) */
typedef struct {
    u16    htype;   /* hardware type = ARP_HTYPE_ETH big-endian */
    u16    ptype;   /* protocol type = ARP_PTYPE_IP  big-endian */
    u8     hlen;    /* hardware address length = 6 */
    u8     plen;    /* protocol address length = 4 */
    u16    oper;    /* ARP_OP_REQUEST / ARP_OP_REPLY big-endian */
    ca_mac_t  sha;     /* sender hardware address */
    ca_ipv4_t spa;     /* sender protocol address */
    ca_mac_t  tha;     /* target hardware address */
    ca_ipv4_t tpa;     /* target protocol address */
} __attribute__((packed)) ca_arp_frame_t;

/** IPv4 header — 20 bytes minimum (RFC 791) */
typedef struct {
    u8     ver_ihl;     /* version[7:4]=4 | IHL[3:0] in 32-bit words */
    u8     dscp_ecn;    /* DSCP[7:2] | ECN[1:0] */
    u16    total_len;   /* total length incl. header big-endian */
    u16    id;          /* identification big-endian */
    u16    flags_frag;  /* flags[15:13] | fragment offset[12:0] big-endian */
    u8     ttl;         /* time to live */
    u8     proto;       /* IP_PROTO_* */
    u16    cksum;       /* header checksum big-endian */
    ca_ipv4_t src;         /* source IPv4 */
    ca_ipv4_t dst;         /* destination IPv4 */
} __attribute__((packed)) ca_ip4_frame_t;

/** UDP header — 8 bytes (RFC 768) */
typedef struct {
    u16    src_port;    /* source port big-endian */
    u16    dst_port;    /* destination port big-endian */
    u16    length;      /* header + data big-endian */
    u16    checksum;    /* 0 = not present (RFC 768) big-endian */
} __attribute__((packed)) ca_udp_frame_t;

/** ICMP echo request/reply header — 8 byte (RFC 792) */
typedef struct {
    u8     type;        /* 8 = echo request, 0 = echo reply */
    u8     code;        /* 0 */
    u16    checksum;    /* big-endian */
    u16    id;          /* identifier big-endian */
    u16    seq;         /* sequence number big-endian */
} __attribute__((packed)) ca_icmp_echo_t;

/** TCP header — 20 byte minimum (RFC 793) */
typedef struct {
    u16    src_port;  /* source port big-endian */
    u16    dst_port;  /* destination port big-endian */
    u32    seq;       /* sequence number big-endian */
    u32    ack_seq;   /* acknowledgement number big-endian */
    u8     data_off;  /* [7:4]=header length in 32-bit words, [3:0]=reserved */
    u8     flags;     /* TCP_FLAG_* */
    u16    window;    /* receive window big-endian */
    u16    checksum;  /* pseudo-header checksum big-endian */
    u16    urg_ptr;   /* urgent pointer big-endian (unused) */
} __attribute__((packed)) ca_tcp_frame_t;

#define TCP_FLAG_FIN   0x01U
#define TCP_FLAG_SYN   0x02U
#define TCP_FLAG_RST   0x04U
#define TCP_FLAG_PSH   0x08U
#define TCP_FLAG_ACK   0x10U
#define TCP_FLAG_URG   0x20U

/* ----------------------------------------------------------------------- */
/* Payload navigation macros                                             */
/* ----------------------------------------------------------------------- */

/** First byte of L2 payload (after 14-byte Ethernet header) */
#define CA_L2_PAYLOAD(l2)           ((u8 *)(l2) + sizeof(ca_l2_frame_t))

/** First byte of L2 VLAN payload (after 18-byte 802.1Q header) */
#define CA_L2_VLAN_PAYLOAD(l2v)     ((u8 *)(l2v) + sizeof(ca_l2_vlan_frame_t))

/** IHL in bytes from IPv4 header */
#define CA_IP4_IHL(ip4)             (((ip4)->ver_ihl & 0x0Fu) << 2)

/** First byte of IPv4 payload (IHL-aware, skips options) */
#define CA_IP4_PAYLOAD(ip4)         ((u8 *)(ip4) + CA_IP4_IHL(ip4))

/** First byte of UDP payload (after 8-byte UDP header) */
#define CA_UDP_PAYLOAD(udp)         ((u8 *)(udp) + sizeof(ca_udp_frame_t))

/** First byte of ICMP payload (after 8-byte ICMP echo header) */
#define CA_ICMP_PAYLOAD(icmp)       ((u8 *)(icmp) + sizeof(ca_icmp_echo_t))

/** TCP header in bytes (from data_off) */
#define CA_TCP_HDR_LEN(tcp)         ((u16)(((tcp)->data_off >> 4U) << 2U))
/** First byte of TCP payload (after header + options) */
#define CA_TCP_PAYLOAD(tcp)         ((u8 *)(tcp) + CA_TCP_HDR_LEN(tcp))

/** VID from 802.1Q TCI field */
#define CA_VLAN_VID(tci)            (ca_ntohs(tci) & 0x0FFFU)

/** PCP from 802.1Q TCI field */
#define CA_VLAN_PCP(tci)            ((ca_ntohs(tci) >> 13U) & 0x07U)

/* ----------------------------------------------------------------------- */
/* L2 filter result                                                         */
/* ----------------------------------------------------------------------- */
typedef enum {
    CA_L2_ACCEPT_UNICAST   = 0,
    CA_L2_ACCEPT_BROADCAST = 1,
    CA_L2_ACCEPT_MULTICAST = 2,
    CA_L2_DROP             = 3,
} ca_l2_result_t;

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/* Initializes the protocol stack with local MAC/IP and registers handlers on MCU. */
void ca_net_proto_init(const ca_net_cfg_t *cfg);

/* Exposed for inline — written only by ca_net_proto_set_local_ip */
extern ca_ipv4_t s_net_proto_local_ip;
/* Updates local IP at runtime (e.g., after DHCP bound). */
static inline void ca_net_proto_set_local_ip(ca_ipv4_t ip)
{
    s_net_proto_local_ip.addr = ip.addr;
}

/* Current local IP (set by ca_net_proto_init / _set_local_ip). */
static inline ca_ipv4_t ca_net_proto_get_local_ip(void)
{
    return s_net_proto_local_ip;
}

/* Registers/removes UDP handlers. fn=NULL removes. Returns false if table is full. */
bool ca_net_proto_bind_udp(u16 port, ca_udp_rx_t fn);

/* Registers global TCP handler (only one handler, manages all PCBs). fn=NULL removes. */
typedef void (*ca_tcp_rx_raw_t)(u16 buf_idx);
void ca_net_proto_bind_tcp(ca_tcp_rx_raw_t fn);

/**
 * Validates TCP header in IPv4 payload.
 * Sets *out to the wire pointer ca_tcp_frame_t (= CA_IP4_PAYLOAD(ip4)).
 * Returns false if proto != TCP or datagram truncated.
 */
bool ca_net_proto_tcp(const ca_ip4_frame_t *ip4, const ca_tcp_frame_t **out);

/**
 * Filters L2 frame: checks length and MAC (unicast/broadcast/multicast).
 * Sets *out to the wire pointer ca_l2_frame_t.
 * If (*out)->ethertype == ETH_P_VLAN, cast to ca_l2_vlan_frame_t*.
 * Returns CA_L2_DROP if frame is too short or not destined for us.
 * local_mac = {0} for promiscuous mode.
 */
ca_l2_result_t ca_net_proto_l2(const u8 *frame, u16 len, ca_mac_t local_mac,
                                const ca_l2_frame_t **out);

/**
 * Validates IPv4/Ethernet ARP in L2 payload.
 * Sets *out to the wire pointer ca_arp_frame_t (= CA_L2_PAYLOAD(l2)).
 * Returns false if EtherType is wrong, len < 28, or htype/ptype/hlen/plen are invalid.
 */
bool ca_net_proto_arp(const ca_l2_frame_t *l2, u16 len,
                      const ca_arp_frame_t **out);

/**
 * Validates IPv4 header in L2 payload (version, IHL, checksum).
 * Sets *out to the wire pointer ca_ip4_frame_t (= CA_L2_PAYLOAD(l2)).
 * Returns false on wrong version, truncation, or incorrect checksum.
 */
bool ca_net_proto_ip4(const ca_l2_frame_t *l2, u16 len,
                      const ca_ip4_frame_t **out);

/**
 * Validates UDP header in IPv4 payload.
 * Sets *out to the wire pointer ca_udp_frame_t (= CA_IP4_PAYLOAD(ip4)).
 * Returns false if proto != UDP or datagram truncated.
 */
bool ca_net_proto_udp(const ca_ip4_frame_t *ip4,
                      const ca_udp_frame_t **out);

/**
 * Validates ICMP echo header (type 0 or 8) in IPv4 payload.
 * Sets *out to the wire pointer ca_icmp_echo_t (= CA_IP4_PAYLOAD(ip4)).
 */
bool ca_net_proto_icmp_echo(const ca_ip4_frame_t *ip4,
                             const ca_icmp_echo_t **out);

/** Internet checksum RFC 1071. Returns 0 on correct data. */
u16 ca_net_cksum(const u8 *buf, u16 len);

/** TCP checksum over the IPv4 pseudo-header + segment (RFC 793). */
u16 ca_net_tcp_cksum(ca_ipv4_t src, ca_ipv4_t dst,
                     const u8 *tcp_seg, u16 tcp_seg_len);

/* Fill in the checksums of an outgoing frame for an iface WITHOUT offload.
 * Called by ca_net_send_on() when the builders left them to the hardware
 * (some registered iface offloads) but this frame leaves from one that does
 * not (NCM). IPv4 header always; ICMP and TCP only on an unfragmented
 * datagram (a fragment's L4 checksum spans the whole datagram). UDP stays 0,
 * as every builder sends it (optional in IPv4, RFC 768). Idempotent. */
void ca_net_proto_csum_fill(u8 *frame, u16 len);

/* Send an IPv4 datagram, fragmenting it when it does not fit the interface MTU.
 *
 * The TX counterpart of ca_ip_defrag_submit(): the stack could reassemble
 * incoming fragments but not produce them, so any datagram too large for one
 * frame was dropped silently by whichever builder had assembled it.
 *
 * The L4 header and the payload are passed SEPARATELY because only the first
 * fragment carries the header - to IP the transport header is just the first
 * l4len bytes of the datagram. Callers keep building their own UDP/ICMP header
 * exactly as before and hand it over here.
 *
 * MTU comes from the interface (ca_net_tx_mtu): NCM refuses frames over 1514,
 * an ETH driver has more room, and the same datagram is legal on one and not
 * on the other.
 *
 * Returns false if a fragment could not be sent. Those already on the wire are
 * not recalled - IP has no such notion - so the peer sees a lost fragment and
 * its reassembly times out.
 */
bool ca_ip_fragment_send(ca_mac_t dst_mac, ca_ipv4_t src_ip, ca_ipv4_t dst_ip,
                         ca_mac_t src_mac, u8 proto,
                         const u8 *l4hdr, u16 l4len,
                         const u8 *payload, u16 plen);

/**
 * Builds ICMP echo reply frame.
 * @param req_l2    L2 wire frame of the request (for src MAC → reply dst MAC)
 * @param req_ip4   IPv4 wire header of the request (for src IP → reply dst IP)
 * @param req_icmp  ICMP echo wire header of the request (type must be 8)
 * @param local_mac our MAC
 * @param local_ip  our IP
 * @param out_frame output buffer (>= 14+20+8+payload_len)
 * @param out_max   buffer size
 * @return          length of produced frame, 0 on error
 */
u16 ca_net_proto_icmp_build_echo_reply(
    const ca_l2_frame_t  *req_l2,
    const ca_ip4_frame_t *req_ip4,
    const ca_icmp_echo_t *req_icmp,
    ca_mac_t  local_mac,
    ca_ipv4_t local_ip,
    u8    *out_frame,
    u16    out_max);

/**
 * Builds ARP reply frame (42 bytes: Eth 14 + ARP 28).
 * @param req        ARP wire request (oper must be ARP_OP_REQUEST)
 * @param local_mac  our MAC
 * @param local_ip   our IP
 * @param out_frame  output buffer >= 42 bytes
 * @return           42 if ok, 0 on error
 */
u16 ca_net_proto_arp_build_reply(const ca_arp_frame_t *req,
                                  ca_mac_t  local_mac,
                                  ca_ipv4_t local_ip,
                                  u8    *out_frame);

/**
 * Builds Gratuitous ARP (GARP) frame to be sent in broadcast.
 * OPER=REQUEST with SPA=TPA=local_ip (→ updates ARP caches of all peers).
 * @param local_mac  our MAC
 * @param local_ip   our IP
 * @param out_frame  output buffer >= 60 bytes (minimum Ethernet frame with padding)
 * @return           60 if ok, 0 on error
 */
u16 ca_net_proto_arp_build_garp(ca_mac_t  local_mac,
                                 ca_ipv4_t local_ip,
                                 u8    *out_frame);

/* ----------------------------------------------------------------------- */
/* DHCP client (requires EN_NET_DHCP)                                      */
/* ----------------------------------------------------------------------- */
#if EN_NET_DHCP

#define DHCP_SERVER_PORT   67U
#define DHCP_CLIENT_PORT   68U
#define DHCP_MSG_DISCOVER  1U
#define DHCP_MSG_OFFER     2U
#define DHCP_MSG_REQUEST   3U
#define DHCP_MSG_ACK       5U
#define DHCP_MSG_NAK       6U

typedef struct {
    ca_ipv4_t offered_ip;   /* yiaddr — IP offered by server */
    ca_ipv4_t server_ip;    /* siaddr — next-server IP (may be 0) */
    ca_ipv4_t server_id;    /* option 54 — Server Identifier (use for REQUEST) */
    ca_ipv4_t subnet_mask;  /* option 1 */
    ca_ipv4_t gateway;      /* option 3 (first router) */
    ca_ipv4_t dns;          /* option 6 (first DNS) */
    ca_ipv4_t ntp_server;   /* option 42 (first NTP server); 0 if not present */
    u32    lease_time;   /* option 51, seconds */
    u32    t1;           /* option 58, seconds; 0 = use lease/2 */
    u32    t2;           /* option 59, seconds; 0 = use lease*7/8 */
    u8     msg_type;     /* DHCP_MSG_OFFER / DHCP_MSG_ACK / DHCP_MSG_NAK */
} ca_dhcp_offer_t;

/**
 * Build DHCP Discover: Eth(14)+IP(20)+UDP(8)+BOOTP(244) = 286 bytes.
 */
u16 ca_net_proto_dhcp_build_discover(ca_mac_t local_mac, u32 xid,
                                      u8 *out_frame, u16 out_max);

/**
 * Build DHCP Request (SELECTING or RENEWING/REBINDING).
 *
 * SELECTING  (initial request after OFFER):
 *   dst_mac = FF:FF:FF:FF:FF:FF, dst_ip = 255.255.255.255
 *   ciaddr  = 0,  offered_ip = yiaddr from OFFER, server_id = opt54 from OFFER
 *
 * RENEWING   (unicast to server after T1):
 *   dst_mac = server MAC, dst_ip = server IP
 *   ciaddr  = our current IP, offered_ip and server_id ignored (ciaddr != 0)
 *
 * REBINDING  (broadcast after T2):
 *   dst_mac = FF:FF:FF:FF:FF:FF, dst_ip = 255.255.255.255
 *   ciaddr  = our current IP
 *
 * When ciaddr.addr != 0 the builder omits options 50 and 54.
 *
 * @return  frame length on success, 0 on error.
 */
u16 ca_net_proto_dhcp_build_request(ca_mac_t    dst_mac,
                                     ca_ipv4_t   dst_ip,
                                     ca_mac_t    local_mac,
                                     ca_ipv4_t   ciaddr,
                                     ca_ipv4_t   offered_ip,
                                     ca_ipv4_t   server_id,
                                     u32      xid,
                                     u8      *out_frame,
                                     u16      out_max);

/**
 * Parse DHCP Offer or ACK from a UDP frame.
 * Also parses options 54 (server_id), 58 (T1), 59 (T2).
 */
bool ca_net_proto_dhcp_parse_offer(const ca_udp_frame_t *udp,
                                    ca_dhcp_offer_t *out);

#endif /* EN_NET_DHCP */

/* ----------------------------------------------------------------------- */
/* NTP client (requires EN_NET_NTP)                                        */
/* ----------------------------------------------------------------------- */
#if EN_NET_NTP

#define NTP_PORT          123U
#define NTP_EPOCH_DELTA   2208988800UL  /* seconds from NTP epoch (1900) to UNIX (1970) */

typedef struct {
    u8  li_vn_mode;      /* Leap Indicator, Version, Mode */
    u8  stratum;         /* Stratum */
    u8  poll;            /* Poll interval */
    i8  precision;       /* Precision */
    u32 root_delay;      /* Root delay (32-bit) */
    u32 root_dispersion; /* Root dispersion (32-bit) */
    u32 ref_id;          /* Reference ID */
    
    /* Reference timestamp (64-bit) */
    u32 ref_ts_s;        /* Seconds */
    u32 ref_ts_f;        /* Fraction */
    
    /* Origin Timestamp (64-bit) - Here the server copies back your XMT */
    u32 org_ts_s;        
    u32 org_ts_f;
    
    /* Receive Timestamp (64-bit) - When the server received the packet */
    u32 rec_ts_s;
    u32 rec_ts_f;
    
    /* Transmit Timestamp (64-bit) - When the server replied (THE TRUE TIME) */
    u32 xmt_ts_s;
    u32 xmt_ts_f;
} __attribute__((packed)) ca_ntp_pkt_t;

/**
 * Build NTP client request: Eth(14)+IP(20)+UDP(8)+NTP(48) = 90 bytes.
 */
u16 ca_net_proto_ntp_build_request(ca_mac_t server_mac, ca_ipv4_t server_ip,
                                    ca_mac_t local_mac,  ca_ipv4_t local_ip,
                                    u8 *out_frame, u16 out_max);

/**
 * Parse NTP server response from a UDP frame.
 * Payload at CA_UDP_PAYLOAD(udp).
 * @return false if payload < 48 bytes or mode != 4 (server)
 */
bool ca_net_proto_ntp_parse_response(const ca_udp_frame_t *udp,
                                      ca_ntp_pkt_t *out);

#endif /* EN_NET_NTP */

/* ----------------------------------------------------------------------- */
/* Syslog client (requires EN_NET_SYSLOG)                                  */
/* ----------------------------------------------------------------------- */
#if EN_NET_SYSLOG

#define SYSLOG_PORT       514U

#define SYSLOG_FAC_KERN   0U
#define SYSLOG_FAC_USER   1U
#define SYSLOG_FAC_LOCAL0 16U
#define SYSLOG_FAC_LOCAL7 23U

#define SYSLOG_SEV_EMERG  0U
#define SYSLOG_SEV_ALERT  1U
#define SYSLOG_SEV_CRIT   2U
#define SYSLOG_SEV_ERR    3U
#define SYSLOG_SEV_WARN   4U
#define SYSLOG_SEV_NOTICE 5U
#define SYSLOG_SEV_INFO   6U
#define SYSLOG_SEV_DEBUG  7U

/**
 * Build a syslog UDP frame (RFC 3164 format: "<PRI>message").
 */
u16 ca_net_proto_syslog_build(ca_mac_t server_mac, ca_ipv4_t server_ip,
                               ca_mac_t local_mac,  ca_ipv4_t local_ip,
                               u8 facility, u8 severity,
                               const char *msg,
                               u8 *out_frame, u16 out_max);

#endif /* EN_NET_SYSLOG */

/* ----------------------------------------------------------------------- */
/* DNS wire format (RFC 1035) — shared by the DNS client and mDNS           */
/*                                                                          */
/* mDNS reuses the DNS record encoding, so these constants must be visible  */
/* whenever either protocol is compiled in — not just under EN_NET_DNS.     */
/* Same rationale as dns_encode_name()/dns_skip_name() in net_proto.c.      */
/* ----------------------------------------------------------------------- */
#if EN_NET_DNS || EN_NET_MDNS

#define DNS_PORT           53U
#define DNS_TYPE_A          1U    /* IPv4 address */
#define DNS_CLASS_IN        1U    /* Internet */
#define DNS_RCODE_OK        0U    /* no error */
#define DNS_RCODE_NXDOMAIN  3U    /* name does not exist */

/* DNS flags bit masks (host order after ca_ntohs) */
#define DNS_FLAGS_QR        0x8000U  /* 1 = response */
#define DNS_FLAGS_RD        0x0100U  /* recursion desired */
#define DNS_FLAGS_RCODE     0x000FU  /* response code mask */

#endif /* EN_NET_DNS || EN_NET_MDNS */

/* ----------------------------------------------------------------------- */
/* DNS client (requires EN_NET_DNS)                                        */
/* ----------------------------------------------------------------------- */
#if EN_NET_DNS

/** DNS header — 12 bytes (RFC 1035 §4.1.1) */
typedef struct {
    u16 id;       /* transaction ID */
    u16 flags;    /* QR|Opcode|AA|TC|RD|RA|Z|RCODE (big-endian) */
    u16 qdcount;  /* question count */
    u16 ancount;  /* answer count */
    u16 nscount;  /* authority RR count */
    u16 arcount;  /* additional RR count */
} __attribute__((packed)) ca_dns_hdr_t;

/** DNS question tail — placed immediately after the encoded QNAME */
typedef struct {
    u16 qtype;    /* DNS_TYPE_A = 1 */
    u16 qclass;   /* DNS_CLASS_IN = 1 */
} __attribute__((packed)) ca_dns_qtail_t;

/** DNS resource record fixed fields — follows variable-length NAME field */
typedef struct {
    u16 type;     /* record type */
    u16 class_;   /* record class */
    u32 ttl;      /* seconds to cache */
    u16 rdlength; /* RDATA length in bytes; RDATA follows immediately */
} __attribute__((packed)) ca_dns_rr_hdr_t;

/** Parsed DNS response — output of ca_net_proto_dns_parse_response() */
typedef struct {
    u16    id;    /* transaction ID (matches query) */
    u8     rcode; /* DNS_RCODE_* */
    ca_ipv4_t ip;    /* resolved IPv4 (first A record); zero if not found */
} ca_dns_response_t;

/**
 * Build DNS A query: Eth(14)+IP(20)+UDP(8)+DNS(12+name+4).
 */
u16 ca_net_proto_dns_build_query(ca_mac_t server_mac, ca_ipv4_t server_ip,
                                  ca_mac_t local_mac,  ca_ipv4_t local_ip,
                                  u16 id, const char *hostname,
                                  u8 *out_frame, u16 out_max);

/**
 * Parse DNS response from a UDP frame.
 * Payload at CA_UDP_PAYLOAD(udp).
 * @return false if not a valid DNS response (QR=0 or payload < 12)
 */
bool ca_net_proto_dns_parse_response(const ca_udp_frame_t *udp,
                                      ca_dns_response_t *out);

#endif /* EN_NET_DNS */

/* ----------------------------------------------------------------------- */
/* mDNS — Multicast DNS (requires EN_NET_MDNS)                             */
/* ----------------------------------------------------------------------- */
#if EN_NET_MDNS

#define MDNS_PORT         5353U
#define MDNS_TTL          120U
#define MDNS_CACHE_FLUSH  0x8000U

/* Multicast MAC and IP for 224.0.0.251 */
#define MDNS_DST_MAC  {0x01U,0x00U,0x5EU,0x00U,0x00U,0xFBU}
#define MDNS_DST_IP   {224U,0U,0U,251U}

/**
 * Build mDNS announcement / response.
 */
u16 ca_net_proto_mdns_build_announce(ca_mac_t local_mac, ca_ipv4_t local_ip,
                                      const char *hostname,
                                      u8 *out_frame, u16 out_max);

/**
 * Parse incoming mDNS query (multicast 224.0.0.251:5353).
 * Payload at CA_UDP_PAYLOAD(udp).
 * @return false if not a valid mDNS query (QR must be 0)
 */
bool ca_net_proto_mdns_parse_query(const ca_udp_frame_t *udp,
                                    const char *our_hostname,
                                    bool *is_for_us);

#endif /* EN_NET_MDNS */

/* ----------------------------------------------------------------------- */
/* TCP builders (requires EN_TCP_SERVICE)                                  */
/* ----------------------------------------------------------------------- */
#if EN_TCP_SERVICE

#define TCP_MSS_DEFAULT  1460U   /* MTU(1500) - IP(20) - TCP(20) */

/**
 * Build TCP SYN (active open): Eth(14)+IP(20)+TCP(24 with MSS opt).
 * @return frame length, 0 on error
 */
u16 ca_net_proto_tcp_build_syn(ca_mac_t dst_mac, ca_ipv4_t dst_ip,
                                ca_mac_t src_mac, ca_ipv4_t src_ip,
                                u16 src_port, u16 dst_port,
                                u32 seq, u16 mss,
                                u8 *out_frame, u16 out_max);

/**
 * Build pure TCP ACK: Eth(14)+IP(20)+TCP(20).
 */
u16 ca_net_proto_tcp_build_ack(ca_mac_t dst_mac, ca_ipv4_t dst_ip,
                                ca_mac_t src_mac, ca_ipv4_t src_ip,
                                u16 src_port, u16 dst_port,
                                u32 seq, u32 ack_seq, u16 window,
                                u8 *out_frame, u16 out_max);

/**
 * Build TCP data segment (PSH+ACK): Eth(14)+IP(20)+TCP(20)+data.
 */
u16 ca_net_proto_tcp_build_data(ca_mac_t dst_mac, ca_ipv4_t dst_ip,
                                 ca_mac_t src_mac, ca_ipv4_t src_ip,
                                 u16 src_port, u16 dst_port,
                                 u32 seq, u32 ack_seq, u16 window,
                                 const u8 *data, u16 data_len,
                                 u8 *out_frame, u16 out_max);

/**
 * Build TCP FIN+ACK: Eth(14)+IP(20)+TCP(20).
 */
u16 ca_net_proto_tcp_build_fin(ca_mac_t dst_mac, ca_ipv4_t dst_ip,
                                ca_mac_t src_mac, ca_ipv4_t src_ip,
                                u16 src_port, u16 dst_port,
                                u32 seq, u32 ack_seq, u16 window,
                                u8 *out_frame, u16 out_max);

/**
 * Build TCP RST: Eth(14)+IP(20)+TCP(20).
 */
u16 ca_net_proto_tcp_build_rst(ca_mac_t dst_mac, ca_ipv4_t dst_ip,
                                ca_mac_t src_mac, ca_ipv4_t src_ip,
                                u16 src_port, u16 dst_port,
                                u32 seq,
                                u8 *out_frame, u16 out_max);

/**
 * Build TCP SYN-ACK frame (server reply to client SYN, 3-way handshake step 2).
 * seq      = server ISN
 * ack_seq  = client_seq + 1
 * mss      = server MSS to advertise
 * Returns frame length (54 bytes with MSS option) or 0 on error.
 */
u16 ca_net_proto_tcp_build_synack(ca_mac_t dst_mac, ca_ipv4_t dst_ip,
                                   ca_mac_t src_mac, ca_ipv4_t src_ip,
                                   u16 src_port, u16 dst_port,
                                   u32 seq, u32 ack_seq, u16 mss,
                                   u8 *out_frame, u16 out_max);

#endif /* EN_TCP_SERVICE */

#endif /* NET_PROTO_H_ */
