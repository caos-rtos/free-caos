// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    net_buf.h
 * @brief   Network buffer pool --- fixed-size 2048-byte buffers, O(1) alloc/free
 *
 * @note    Pool of NET_BUF_COUNT (64) buffers placed in .net_pool section.
 *          On H7xx/H5xx the linker maps .net_pool to D2 SRAM1 (0x30000000,
 *          128KB) configured as Non-cacheable via MPU in ca_net_buf_pool_init().
 *          On other families the pool lands in default RAM (.bss).
 *
 *          Buffer layout (VPP-inspired):
 *            [  64 B metadata  |  1984 B data area  ]  = 2048 B total
 *          data_off points to the start of the current payload inside data[].
 *          The first NET_BUF_HEADROOM bytes of data[] are reserved headroom
 *          for prepending L2/L3 headers without copying.
 *
 *          ISR-safe: alloc/free use PRIMASK critical section.
 ******************************************************************************
 */

#ifndef NET_BUF_H_
#define NET_BUF_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_NET

/* ----------------------------------------------------------------------- */
/* Pool constants                                                           */
/* ----------------------------------------------------------------------- */
#define NET_BUF_SIZE       2048U          /* total bytes per buffer           */
#define NET_BUF_HDR_SIZE   64U            /* metadata header                  */
#define NET_BUF_DATA_SIZE  (NET_BUF_SIZE - NET_BUF_HDR_SIZE)  /* 1984        */
#define NET_BUF_HEADROOM   128U           /* reserved prepend area in data[]  */
/* NET_BUF_COUNT lives in hw/<family>/<mcu>/objmap.h, and NOWHERE ELSE.
 *
 * It used to be a -D in three separate places - hw/<fam>/<mcu>/caos_<mcu>.cmake,
 * examples/<fam>/<mcu>/CMakeLists.txt and workspace/<fam>/<mcu>/CMakeLists.txt -
 * and the three did not agree. On F407 and F446 the library was compiled with 8
 * and the examples with 16, so net_buf.c held s_pool[8] while ex33 looped to 16,
 * sized arrays at 16 and compared free+held against 16: "FAIL leak detected!" on
 * a healthy board, and a read past the end of the pool. Nothing in that is
 * visible at compile time - each translation unit is self-consistent.
 *
 * objmap.h is already per-MCU (one file per part, with its own include guard),
 * so the value cannot diverge between two parts of the same family and MCU_ID
 * is not needed to tell them apart. Moved there 2026-09-17. */
#ifndef NET_BUF_COUNT
#error "NET_BUF_COUNT must be defined in hw/<family>/<mcu>/objmap.h"
#endif
#define NET_BUF_INVALID    0xFFFFU        /* sentinel: "no buffer"            */

/* MPU region used for net pool. Defined per-MCU in objmap.h (must not
 * conflict with mpu.h kernel canary regions nor with other driver regions,
 * e.g. USB_MPU_REGION/ETH_MPU_REGION) — the MPU region layout is a hardware
 * resource that depends on the number of regions the MCU actually has. */
#ifndef NET_MPU_REGION
#error "NET_MPU_REGION must be defined in objmap.h"
#endif

/* ----------------------------------------------------------------------- */
/* Buffer structure                                                         */
/* ----------------------------------------------------------------------- */

/* L3 protocol hint stored in .proto */
#define NET_PROTO_NONE  0x00U
#define NET_PROTO_ARP   0x01U
#define NET_PROTO_IP4   0x02U
#define NET_PROTO_IP6   0x03U

/* .flags bits */
#define NET_BUF_F_IN_USE  (1U << 0)
#define NET_BUF_F_TX      (1U << 1)
#define NET_BUF_F_RAW     (1U << 2)   /* send as is: no checksum insertion by
                                         the MAC, no software fill
                                         (ca_net_send_raw) */

/* RX parse cache — filled progressively by the decode pipeline:
 *   ca_net_process()          → l3_off, vlan_id, vlan_pcp
 *   ca_net_proto_arp_handler  → src_ip (spa), dst_ip (tpa)
 *   ca_net_proto_ip4_handler  → src_ip, dst_ip, ip_proto, l4_off
 *                             → src_port, dst_port (after UDP decode)
 * All offsets are relative to frame start (b->data + b->data_off). */
typedef struct {
    u16    l3_off;   /* frame offset to L3 header: 14 no-VLAN, 18 with 802.1Q */
    u16    l4_off;   /* frame offset to L4 header: l3_off + IP IHL; 0 if none */
    ca_ipv4_t src_ip;   /* IPv4 src (spa for ARP, 0 if non-IP)                   */
    ca_ipv4_t dst_ip;   /* IPv4 dst (tpa for ARP, 0 if non-IP)                   */
    u16    src_port; /* UDP/TCP source port (0 if non UDP/TCP)                */
    u16    dst_port; /* UDP/TCP dest   port (0 if non UDP/TCP)                */
    u16    vlan_id;  /* 802.1Q VID (0 = untagged)                             */
    u8     vlan_pcp; /* 802.1Q PCP (0-7)                                      */
    u8     ip_proto; /* IP_PROTO_ICMP=1 / UDP=17 / TCP=6; 0 if non-IP        */
    u8     iface;    /* ingress iface id (ca_net_iface_t.id): replies go back
                        out of it                                            */
} ca_net_buf_rx_t;                                               /* 24 bytes */

/* TX routing info — filled by upper layer before ca_net_send() */
typedef struct {
    ca_ipv4_t dst_ip;   /* IPv4 destination address                              */
    u16    dst_port; /* UDP destination port                                  */
    u16    src_port; /* UDP source port                                       */
    ca_mac_t  dst_mac;  /* resolved destination MAC                              */
} ca_net_buf_tx_t;                                               /* 14 bytes */

typedef union {
    ca_net_buf_rx_t rx;   /* RX path: filled by L2→L3→L4 decode             */
    ca_net_buf_tx_t tx;   /* TX path: filled by upper layer                  */
    u8              raw[52];
} ca_net_buf_meta_t;                                             /* 52 bytes */

typedef struct {
    /* ---- 64 byte metadata ---- */
    u16               next;      /* chained buffer index, NET_BUF_INVALID=end */
    u16               data_off;  /* byte offset from data[0] to payload start */
    u16               data_len;  /* payload length in bytes                   */
    u8                flags;     /* NET_BUF_F_*                               */
    u8                proto;     /* NET_PROTO_* set by L2 dispatcher          */
    u32               timestamp; /* ca_os_tick() at frame reception           */
    ca_net_buf_meta_t meta;      /* L2/L3/L4 parse cache + TX routing info    */
    /* ---- 1984 byte data area ---- */
    u8                data[NET_BUF_DATA_SIZE];
} ca_net_buf_t;

_Static_assert(sizeof(ca_net_buf_t) == NET_BUF_SIZE, "ca_net_buf_t must be 2048 bytes");

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/* Initialise pool: chain all buffers into free list, configure MPU NCacheable
 * on Cortex-M7/M33 (if D-cache present). Must be called once before any
 * alloc/free and after ca_mpu_init(). */
void ca_net_buf_pool_init(void);

/* Allocate one buffer.  Returns its index (0..NET_BUF_COUNT-1) or
 * NET_BUF_INVALID when pool is empty.  ISR-safe. */
u16           ca_net_buf_alloc(void);

/* Release one reference; the buffer returns to the pool when the last one
 * goes.  ISR-safe. */
void          ca_net_buf_free(u16 idx);

/* Take one more reference on a buffer in use, so two owners can hold it at
 * once (TCP store + driver TX). Each owner frees it once. The data is SHARED:
 * neither owner may write it while the other holds it. false if the buffer is
 * not in use or the count is saturated.  ISR-safe. */
bool          ca_net_buf_ref(u16 idx);

/* References beyond the first, summed over the pool: a buffer counted by two
 * owners appears this many extra times in their totals.  ISR-safe. */
u16           ca_net_buf_shared_count(void);

/* Convert index --- pointer.  No bounds check --- caller must ensure idx is valid. */
ca_net_buf_t *ca_net_buf_ptr(u16 idx);

/* Number of currently free buffers (informational, not ISR-safe). */
u16           ca_net_buf_free_count(void);

/* free() calls refused because the buffer was already free. Must stay 0:
   any other value means two owners were handed the same buffer. */
u32           ca_net_buf_double_free_count(void);

/* Return address of the first refused free, 0 if none. Resolve with:
   arm-none-eabi-addr2line -f -e <elf> 0x<addr> */
u32           ca_net_buf_double_free_caller(void);

/* Pool pressure over a measurement window, for sizing NET_BUF_COUNT.
 * free_min is what decides it: the pool has to be big enough for the WORST
 * instant of a transfer, which a reading taken afterwards never shows. */
typedef struct {
    u16 total;       /* NET_BUF_COUNT                                        */
    u16 free_now;    /* free buffers at the time of the call                 */
    u16 free_min;    /* fewest ever free since the last reset  <-- the number */
    u32 alloc_calls; /* allocations attempted since the last reset           */
    u32 alloc_fail;  /* of those, refused: the pool was empty                */
} ca_net_buf_pressure_t;

void          ca_net_buf_pressure(ca_net_buf_pressure_t *out);

/* Restart the window. free_min restarts from the CURRENT free count, not from
 * total: what is held right now is not headroom the next run will have. */
void          ca_net_buf_pressure_reset(void);

#endif /* EN_NET */
#endif /* NET_BUF_H_ */
