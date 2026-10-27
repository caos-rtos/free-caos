// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

#ifndef CAOS_INFO_H
#define CAOS_INFO_H

#include "caos.h"

/**
 * @brief Read-only snapshot of the caOS build configuration.
 *
 * @details Gathers in a single structure every configuration choice fixed at
 * compile time, plus the identity of the build:
 *   - the feature flags and numeric parameters of @c os_config.h
 *     (I/O subsystems, peripherals, NET stack, crypto, OS primitives, CLI);
 *   - the MCU-specific sizing values of @c objmap.h
 *     (bus clocks, @c MAX_TASK / @c MAX_TIMER, stack sizes,
 *     @c DEEP_GUARD_MAX, USB endpoint map, board markers);
 *   - the identity of the build: MCU family (@c mcu), chip id
 *     (@c mcu_id) and version string (@c build_ver = git hash + date).
 *
 * @par Purpose — comparing caOS configurations/versions
 * The main use is to get a fingerprint of the configuration the firmware was
 * built with, so that one can tell whether **two different caOS builds were
 * generated with the same configuration**: compare the fields (or compute a
 * hash over the structure). The field layout is deliberately identical on
 * every MCU family — values that do not apply to a given MCU stay 0 — so the
 * comparison stays meaningful across different MCUs too.
 *
 * @warning **Compiler flags are NOT here, on purpose.** The optimisation level
 * (@c -O0/-O2/-O3), @c NDEBUG, language/warning flags and the like are set by
 * the **build environment and the user** (CMake / @c CMakeLists.txt,
 * toolchain), not by @c os_config.h. They are therefore outside this
 * structure: two builds with an identical struct may still differ in their
 * compiler flags. For a complete comparison the build flags of the
 * environment have to be checked separately.
 *
 * @note Read-only for the application: changing its values at runtime does
 * not change the real configuration (it mirrors the static @c #defines).
 * Numeric fields of disabled subsystems and objmap fields not present on the
 * current MCU are 0.
 *
 * @see ca_caos_get_config()
 */
typedef struct {
    /* --- Build identity (def_mcu / cmake) — first fields of the struct --- */
    /* Inline strings (NOT pointers): the content lives in the struct, so
     * comparing/copying/hashing the fingerprint is meaningful.
     * build_ver is the FIRST field (offset 0): ca_caos_config_hash excludes it
     * by hashing a single contiguous range starting right after it. */
    char build_ver[56]; /* BUILD_VER: "<git-hash[+dev]> - <YYYY-MM-DD HH:MM:SS> - <Debug|Release>[ - FREE]"
                         * worst case 51 chars + NUL ('+dev', 'Release', FREE edition) */
    char mcu[8];        /* family string, e.g. "H7xx" (D_TO_STR(MCU)) */
    u16  mcu_id;        /* chip id, e.g. 753 (MCU_ID) */
    /* --- I/O subsystems (os_config.h) --- */
    u8  itm;
    u8  dbg_uart;
    u8  uart;
    /* USB */
    u8  usb;
    u8  usb_cdc;
    u16 usb_pid_base_cdc;
    u8  usb_msc;
    u16 usb_pid_base_msc;
    u8  usb_ncm;
    u16 usb_pid_base_ncm;
    u8  usb_dfu;
    u16 usb_pid_base_dfu;
    /* stdout */
    u8  stdout_en;
    u8  stdout_on_uart;
    u8  stdout_on_usb_cdc;
    u8  stdout_on_itm;
    /* CLI */
    u8  cli;
    u8  cli_capture;
    u8  stdin_on_uart;
    u8  stdin_on_usb_cdc;
    /* sensor */
    u8  sensor;
    /* --- Peripherals (os_config.h) --- */
    u8  spi;
    u8  i2c;
    u8  adc;
    u8  dma;
    u8  exti;
    u8  tamp;
    u8  fmac;
    u8  sai;
    u8  spdifrx;
    u8  dma2d;
    u8  jpeg;
    u8  dac;
    u8  flash;
    u8  qspi;
    u8  sdio;
    u8  timer;
    u8  wdt;
    u8  can;
    u8  eth;
    u8  eth_chk_hw;
    u8  rtc;
    /* --- NET stack (os_config.h) --- */
    u8  net;
    u8  net_dhcp;
    u8  net_ntp;
    u8  net_syslog;
    u8  net_dns;
    u8  net_mdns;
    u8  packet_tracer;
    u8  arp_service;
    /* Tuning parameters follow the flag that guards them: this struct is the
     * canonical ordering of the configuration (tools/caos_config.py derives
     * the emitted sequence from it). */
    u32 arp_entry_ttl;
    u8  arp_retry_n;
    u16 arp_retry_ms;
    u8  route_service;
    u8  udp_server;
    u8  udp_client;
    u8  dhcp_service;
    u8  dhcp_discover_retry_n;
    u16 dhcp_offer_timeout_ms;
    u16 dhcp_request_timeout_ms;
    u16 dhcp_renew_timeout_ms;
    u16 dhcp_retry_backoff_ms;
    u32 dhcp_bind_timeout_ms;
    u8  ntp_service;
    u16 ntp_response_timeout_ms;
    u8  ntp_retry_n;
    u16 ntp_retry_backoff_ms;
    u8  dns_service;
    u16 dns_response_timeout_ms;
    u8  dns_retry_n;
    u8  mdns_service;
    u8  syslog_service;
    u8  tcp_service;
    u16 tcp_accept_timeout_ms;
    u16 tcp_rto_initial_ms;
    u16 tcp_rto_max_ms;
    u8  tcp_max_retries;
    u16 tcp_time_wait_ms;
    u16 tcp_timer_tick_ms;
    u16 tcp_arp_timeout_ms;
    u16 tcp_connect_timeout_ms;
    u8  mqtt_service;
    u8  modbus_service;
    u8  iperf_service;
    u32 iperf_default_duration_ms;
    u16 iperf_idle_timeout_ms;
    u8  http_service;
    /* --- Crypto / compute (os_config.h) --- */
    u8  crc;
    u8  rng;
    u8  hash;
    u8  aes;
    /* --- OS primitives (os_config.h) --- */
    u8  sw_timer;
    u8  event_grp;
    u8  notify;
    u8  message;
    u8  stream;
    u8  measure;
    u8  keys_store;
    u8  pool;
    u8  block_pool;
    u8  rwlock;
    u8  barrier;
    /* --- Endian (os_config.h) --- */
    u8  big_endian;
    u8  little_endian;
    /* --- CLI sub-flags (os_config.h) --- */
    u8  cli_arp;
    u8  cli_dhcp;
    u8  cli_ntp;
    u8  cli_dns;
    u8  cli_mdns;
    u8  cli_syslog;
    u8  cli_tcp;
    u8  cli_mqtt;
    u8  cli_modbus;
    u8  cli_iperf;
    u8  cli_http;
    u8  cli_trace;
    u8  cli_udp_srv;
    u8  cli_rtc;
    u8  cli_eth;
    u8  cli_net;
    u8  cli_route;
    u8  cli_gpio;
    u8  cli_uart;
    u8  cli_adc;
    u8  cli_dac;
    u8  cli_i2c;
    u8  cli_flash;
    u8  cli_timer;
    u8  cli_dma;
    u8  cli_spi;
    u8  cli_wdt;
    u8  cli_rng;
    u8  cli_crc;
    u8  cli_kstore;
    u8  cli_sdio;
    u8  cli_qspi;
    u8  cli_can;
    /* --- Extended drivers (os_config.h) --- */
    u8  lptim;
    u8  lpuart;
    u32 lpuart_std_baud;
    u8  lpuart_std_wake;
    u8  spi_dma;
    u8  uart_dma;
    u8  fat;
    u8  ip_defrag;
    u8  ip_defrag_max_sessions;
    u16 ip_defrag_timeout_ms;
    u16 ip_defrag_max_payload;
    /* --- Power (os_config.h) --- */
    u8  pwr;
    u8  pwr_dbg;
    /* --- USB varianti (os_config.h) --- */
    u8  usb_fs;
    u8  usb_hs;
    u8  usb_custom;
    u8  usb_dbg;
    /* --- Crypto primitive SW (os_config.h) --- */
    u8  sha256;
    u8  sha512;
    u8  x25519;
    u8  ed25519;
    u8  sha256_test;
    u8  sha512_test;
    u8  x25519_test;
    u8  ed25519_test;
    u8  aes_gcm_sw_test;
    /* --- Net/crypto services (os_config.h) --- */
    u8  ssh_service;
    u16 ssh_listen_port;
    u8  ssh_test;
    u8  ssh_trace;
    u8  tftp_service;
    u16 tftp_block_timeout_ms;
    u8  tftp_retry_n;
    u8  tftp_max_xfer;
    u8  keystore_service;
    u8  aes_channel_service;
    /* --- Extended stdout/stdin routing (os_config.h) --- */
    u8  stdout_on_lpuart;
    u8  stdin_on_lpuart;
    u8  stdin_on_usb_ncm;
    /* --- Extended CLI sub-flags (os_config.h) --- */
    u8  cli_config;
    u8  cli_lptim;
    u8  cli_pwr;
    u8  cli_tftp;
    u8  cli_ssh;
    u8  cli_ksvc;
    u8  cli_sha256;
    u8  cli_sha512;
    u8  cli_x25519;
    u8  cli_ed25519;
    /* --- Numeric: clocks (objmap.h) --- */
    u32 sysclk_hz;
    u32 hclk_hz;
    u32 pclk1_hz;
    u32 pclk2_hz;
    /* --- Numeric: scheduler (objmap.h) --- */
    u8  max_task;
    u8  max_timer;
    u8  dump_buffer_size;
    u32 os_tick_us;
    /* --- Numeric: stack sizes (objmap.h) --- */
    u32 stack_min;
    u32 stack_small;
    u32 stack_medium;
    u32 stack_large;
    u32 stack_xlarge;
    /* --- Numeric: UART debug (os_config.h) --- */
    u8  uart_dbg_num;
    u32 uart_dbg_baudrate;
    /* --- Numeric: net sizes / counts (os_config.h) --- */
    u8  packet_tracer_ring_size;
    u8  arp_cache_size;
    u8  net_route_max;
    u8  pool_queue_size;
    u8  udpsrv_max_clients;
    u8  udpsrv_ring_depth;
    u8  udpcli_max_handles;
    u8  udpcli_ring_depth;
    u8  tcp_max_conn;
    u8  tcp_max_listen;
    u8  tcp_max_inflight;
    u8  tcp_rx_ring_depth;
    u8  ntp_server_n;
    u32 ntp_poll_ms;
    u8  dns_pending_max;
    u8  syslog_queue_size;
    u8  syslog_msg_max;
    u16 mqtt_rx_buf_size;
    u16 mqtt_keepalive_sec;
    u32 modbus_timeout_ms;
    u16 modbus_rx_buf_size;
    u16 iperf_default_port;
    u16 http_default_port;
    u16 http_req_buf_size;
    u16 http_resp_buf_size;
    /* --- Numeric: extra clocks (objmap.h, per-MCU) --- */
    u32 pclk3_hz;
    u32 pclk4_hz;
    /* --- Numeric: MPU stack guard / kernel stack (objmap.h) --- */
    u32 deep_guard_max;
    u32 stack_kernel;
    /* --- Numeric: USB endpoint map (objmap.h) --- */
    u8  usb_ep_max;
    u8  usb_ep0_mps;
    u16 usb_ep0_tx_buf;
    u8  usb_ep0_xfr_max;
    u8  usb_cdc_ep_data;
    u8  usb_cdc_ep_notif;
    u8  usb_msc_ep_data;
    u8  usb_ncm_ep_data;
    u8  usb_ncm_ep_notif;
    /* --- Board / variant config (objmap.h, per-MCU) --- */
    u8  uart_stdout_num;
    u8  mcu_no_eth;
} ca_caos_config_status;

ca_caos_config_status *ca_caos_get_config(void);

#if EN_SHA256
#define CA_CONFIG_HASH_LEN 32u   /* SHA-256 digest length */

/**
 * @brief SHA-256 fingerprint of the build configuration.
 *
 * @details Computes a SHA-256 digest (software, @ref ca_sha256) over the
 * content of @ref ca_caos_config_status, **excluding the @c build_ver field**
 * (git hash + timestamp, which change at every build). The digest keeps the
 * MCU identity (@c mcu / @c mcu_id) and every configuration flag/value.
 *
 * Intended consequence: **two builds with the same configuration produce the
 * same hash**, even if compiled at different times or from different commits.
 * It changes only when a configuration choice (os_config.h / objmap.h) or the
 * MCU changes. Useful as a compact fingerprint to return over network/serial/
 * USB and compare between systems. For the detailed diff use the CLI command
 * @c config (text) with diff/meld.
 *
 * @param[out] digest buffer of at least @ref CA_CONFIG_HASH_LEN (32) bytes.
 * @note Compiler flags (-O, NDEBUG, ...) are not in the configuration and so
 *       do not enter the digest: they belong to the build environment.
 */
void ca_caos_config_hash(u8 *digest);
#endif /* EN_SHA256 */

#endif /* CAOS_INFO_H */
