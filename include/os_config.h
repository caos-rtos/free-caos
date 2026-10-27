/* caOS FREE - THIS IS THE CONFIGURATION THE LIBRARIES WERE BUILT WITH.
 * It is fixed: editing it does not change the precompiled libraries, and a
 * value that differs from theirs breaks the build in silence (struct
 * layouts). Values your application must provide are symbols, not defines:
 *   SSH: ca_ssh_authorized_pub[32] - generate a key pair
 *        (ssh-keygen -t ed25519), keep the private key on the client and
 *        define the 32 bytes of the public key in your .c (ssh_service.h).
 *        Here it is all zeros: no client is authorized.
 */
/* =============================================================================
 * O.S. COMPONENTS CONFIGURATION
 * free
 *
 * GENERATED FILE by tools/caos_config.py - do NOT edit by hand.
 * Symbol model: BASELINE in tools/caos_model.py
 * Regenerate with:  python3 tools/caos_config.py --purge <file>
 * ============================================================================= */

#ifndef OS_COMPONETS_HEADER
#define OS_COMPONETS_HEADER

/* ===========================================================================
 * DEBUG ITM CONFIGURATION (stderr)
 * =========================================================================== */
#define ENABLE_ITM                   0

/* ===========================================================================
 * DEBUG UART CONFIGURATION (stderr)
 * =========================================================================== */
#undef  ENABLE_DBG_UART
#define ENABLE_DBG_UART              1
#define EN_STLINK_WAIT_READY         1
#if ( ENABLE_DBG_UART )
#define UART_DBG_BAUDRATE            CA_UART_BAUD_115200
#endif

/* ===========================================================================
 * UART CONFIGURATION
 * =========================================================================== */
#define ENABLE_UART                  1
#define EN_LPUART                    1
#define LPUART_STD_BAUD              115200U
#define LPUART_STD_WAKE              1
#define EN_UART_DMA                  0

/* ===========================================================================
 * USB CONFIGURATION
 * =========================================================================== */
#define EN_USB                       1
#if ( EN_USB )
#define EN_USB_CDC                   1
#define USB_PID_BASE_CDC             0x0010U
#define EN_USB_MSC                   1
#define USB_PID_BASE_MSC             0x0020U
#define EN_USB_NCM                   1
#define USB_PID_BASE_NCM             0x0030U
#define EN_USB_DFU                   1
#define USB_PID_BASE_DFU             0x0040U
#define EN_USB_FS                    1
#define EN_USB_HS                    0
#define EN_USB_CUSTOM                1
#define EN_USB_DBG                   0
#define USB_PID_BASE_CUSTOM          0x0000U
#define USB_VID                      0xCA01U
#define USB_BCD_DEVICE               0x0100U
#define USB_STR_MANUFACTURER         "caOS"
#define USB_STR_PRODUCT              "caOS USB Device"
#define USB_STR_SERIAL               "0001"
#endif

/* ===========================================================================
 * PERIPHERALS CONFIGURATION
 * =========================================================================== */
#define ENABLE_STDOUT                1
#define EN_LED                       1
#define ENABLE_SENSOR                1
#define EN_SPI                       1
#define EN_I2C                       1
#define EN_ADC                       1
#define EN_DMA                       1
#define EN_EXTI                      1
#define EN_TAMP                      0
#define EN_FMAC                      0
#define EN_SAI                       1
#define EN_SPDIFRX                   1
#define EN_DMA2D                     1
#define EN_JPEG                      1
#define EN_DAC                       1
#define EN_FLASH                     1
#define EN_QSPI                      1
#define EN_SDIO                      1
#define EN_TIMER                     1
#define EN_WDT                       1
#define EN_CAN                       0
#define EN_RTC                       1
#define EN_SW_TIMER                  1
#define EN_MESSAGE                   1
#define EN_STREAM                    1
#define EN_MEASURE                   1
#define EN_POOL                      1
#define EN_BLOCK_POOL                1
#define EN_RWLOCK                    1
#define EN_BARRIER                   1
#define EN_BIG_E                     1
#define EN_LITTLE_E                  1
#define EN_LPTIM                     1
#define EN_SPI_DMA                   0
#define EN_FAT                       1
#define EN_PWR                       1
#define EN_PWR_DBG                   0
#define EN_BOOT_IMAGE                1
#if ( ENABLE_STDOUT )
#define STDOUT_ON_UART               1
#define STDOUT_ON_USB_CDC            0
#define STDOUT_ON_ITM                0
#define STDOUT_ON_LPUART             0
#endif
#if ( EN_FAT )
#define EN_FAT_SERVICE               1
#endif
#if ( EN_FAT && EN_FAT_SERVICE )
#define EN_FAT_CRYPT                 1
#endif

/* ===========================================================================
 * NETWORK CONFIGURATION
 * =========================================================================== */
#define EN_ETH                       1
#ifdef MCU_NO_ETH
#undef  EN_ETH
#define EN_ETH                       0
#endif
#define EN_ETH_CHK_HW                1
#define EN_NET                       1
#if !EN_ETH && !(EN_USB && EN_USB_NCM)
#undef  EN_NET
#define EN_NET                       0
#endif
#if ( EN_ETH )
#define EN_ETH_DBG                   1
#endif
#if ( EN_NET )
#define EN_NET_FILTER                1
#define EN_NET_DHCP                  1
#define EN_NET_NTP                   1
#define EN_NET_SYSLOG                1
#define EN_NET_DNS                   1
#define EN_NET_MDNS                  1
#define EN_PACKET_TRACER             1
#define EN_ARP_SERVICE               1
#define EN_ROUTE_SERVICE             1
#define EN_UDP_SERVER                1
#define EN_UDP_CLIENT                1
#define EN_DHCP_SERVICE              1
#define EN_NTP_SERVICE               1
#define EN_DNS_SERVICE               1
#define EN_MDNS_SERVICE              1
#define EN_SYSLOG_SERVICE            1
#define EN_TCP_SERVICE               1
#define EN_MQTT_SERVICE              1
#define EN_MODBUS_SERVICE            1
#define EN_IPERF_SERVICE             1
#define EN_HTTP_SERVICE              1
#define EN_HTTPS_SERVICE             0
#define EN_IP_DEFRAG                 1
#define EN_SSH_SERVICE               1
#define EN_TFTP_SERVICE              1
#endif
#if ( EN_ARP_SERVICE )
#define ARP_ENTRY_TTL                30000U
#define ARP_RETRY_N                  3U
#define ARP_RETRY_MS                 500U
#define ARP_CACHE_SIZE               8U
#endif
#if ( EN_DHCP_SERVICE )
#define DHCP_DISCOVER_RETRY_N        3U
#define DHCP_OFFER_TIMEOUT_MS        4000U
#define DHCP_REQUEST_TIMEOUT_MS      4000U
#define DHCP_RENEW_TIMEOUT_MS        10000U
#define DHCP_RETRY_BACKOFF_MS        5000U
#define DHCP_BIND_TIMEOUT_MS         60000U
#endif
#if ( EN_DNS_SERVICE )
#define DNS_RESPONSE_TIMEOUT_MS      3000U
#define DNS_RETRY_N                  2U
#define DNS_PENDING_MAX              4U
#endif
#if ( EN_HTTPS_SERVICE )
#define HTTPS_DEFAULT_PORT           443U
#endif
#if ( EN_HTTP_SERVICE )
#define HTTP_DEFAULT_PORT            80U
#define HTTP_REQ_BUF_SIZE            2048U
#define HTTP_RESP_BUF_SIZE           4096U
#endif
#if ( EN_IPERF_SERVICE )
#define IPERF_DEFAULT_DURATION_MS    10000U
#define IPERF_IDLE_TIMEOUT_MS        3000U
#define IPERF_DEFAULT_PORT           5001U
#endif
#if ( EN_IP_DEFRAG )
#define IP_DEFRAG_MAX_SESSIONS       8U
#define IP_DEFRAG_TIMEOUT_MS         15000U
#define IP_DEFRAG_MAX_PAYLOAD        2048U
#endif
#if ( EN_MODBUS_SERVICE )
#define MODBUS_TIMEOUT_MS            1000U
#define MODBUS_RX_BUF_SIZE           270U
#endif
#if ( EN_MQTT_SERVICE )
#define MQTT_RX_BUF_SIZE             256U
#define MQTT_KEEPALIVE_SEC           60U
#endif
#if ( EN_NTP_SERVICE )
#define NTP_RESPONSE_TIMEOUT_MS      5000U
#define NTP_RETRY_N                  3U
#define NTP_RETRY_BACKOFF_MS         10000U
#define NTP_SERVER_N                 2U
#define NTP_POLL_MS                  64000U
#endif
#if ( EN_PACKET_TRACER )
#define PACKET_TRACER_RING_SIZE      8U
#endif
#if ( EN_ROUTE_SERVICE )
#define NET_ROUTE_MAX                8U
#endif
#if ( EN_SSH_SERVICE )
#define SSH_LISTEN_PORT              22U
#define EN_SSH_TEST                  1
#define EN_SSH_TRACE                 1
#endif
#if ( EN_SYSLOG_SERVICE )
#define SYSLOG_QUEUE_SIZE            8U
#define SYSLOG_MSG_MAX               128U
#endif
#if ( EN_TCP_SERVICE )
#define EN_TCP_TRACE                 0
#define TCP_TCP_ACCEPT_TIMEOUT_MS    10000U
#define TCP_RTO_INITIAL_MS           1000U
#define TCP_RTO_MAX_MS               16000U
#define TCP_MAX_RETRIES              5U
#define TCP_TIME_WAIT_MS             4000U
#define TCP_CLOSE_WAIT_MS            10000U
#define TCP_TIMER_TICK_MS            100U
#define TCP_ARP_TIMEOUT_MS           2000U
#define TCP_CONNECT_TIMEOUT_MS       5000U
#define TCP_MAX_CONN                 8U
#define TCP_MAX_LISTEN               2U
#define TCP_MAX_INFLIGHT             16U
#define TCP_RX_RING_DEPTH            16U
#endif
#if ( EN_TFTP_SERVICE )
#define TFTP_BLOCK_TIMEOUT_MS        5000U
#define TFTP_MAX_XFER                2U
#define TFTP_RETRY_N                 3U
#endif
#if ( EN_UDP_CLIENT )
#define UDPCLI_MAX_HANDLES           4U
#define UDPCLI_RING_DEPTH            4U
#endif
#if ( EN_UDP_SERVER )
#define UDPSRV_MAX_CLIENTS           4U
#define UDPSRV_RING_DEPTH            8U
#endif

/* ===========================================================================
 * CRYPTO CONFIGURATION
 * =========================================================================== */
#define EN_CRC                       1
#define EN_RNG                       1
#define EN_HASH                      1
#define EN_AES                       1
#define EN_KEYS_STORE                1
#define EN_SHA256                    1
#define EN_SHA512                    1
#define EN_X25519                    1
#define EN_ED25519                   1
#define EN_ECDSA_P256                0
#define EN_TLS                       0
#define EN_SHA256_TEST               0
#define EN_SHA512_TEST               0
#define EN_X25519_TEST               0
#define EN_ED25519_TEST              0
#define EN_ECDSA_P256_TEST           0
#define EN_AES_GCM_SW_TEST           0
#define EN_KEYSTORE_SERVICE          1
#define EN_AES_CHANNEL_SERVICE       1
#define SSH_AUTHORIZED_ED25519_PUB   { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }

/* ===========================================================================
 * CLI CONFIGURATION
 * =========================================================================== */
#define ENABLE_CLI                   1
#if ( ENABLE_CLI )
#define EN_CLI_CAPTURE               1
#define STDIN_ON_UART                1
#define STDIN_ON_USB_CDC             0
#define STDIN_ON_LPUART              0
#define STDIN_ON_USB_NCM             0
#define EN_CLI_LED                   1
#define EN_CLI_ARP                   1
#define EN_CLI_DHCP                  1
#define EN_CLI_NTP                   1
#define EN_CLI_DNS                   1
#define EN_CLI_MDNS                  1
#define EN_CLI_SYSLOG                1
#define EN_CLI_TCP                   1
#define EN_CLI_MQTT                  1
#define EN_CLI_MODBUS                1
#define EN_CLI_IPERF                 1
#define EN_CLI_HTTP                  1
#define EN_CLI_HTTPS                 0
#define EN_CLI_TRACE                 0
#define EN_CLI_UDP_SRV               1
#define EN_CLI_RTC                   1
#define EN_CLI_ETH                   1
#define EN_CLI_NET                   1
#define EN_CLI_ROUTE                 1
#define EN_CLI_GPIO                  1
#define EN_CLI_UART                  1
#define EN_CLI_ADC                   1
#define EN_CLI_DAC                   1
#define EN_CLI_I2C                   1
#define EN_CLI_FLASH                 1
#define EN_CLI_TIMER                 1
#define EN_CLI_DMA                   1
#define EN_CLI_SPI                   1
#define EN_CLI_WDT                   1
#define EN_CLI_RNG                   1
#define EN_CLI_CRC                   1
#define EN_CLI_KSTORE                1
#define EN_CLI_SDIO                  1
#define EN_CLI_QSPI                  1
#define EN_CLI_CAN                   0
#define EN_CLI_CONFIG                1
#define EN_CLI_LPTIM                 1
#define EN_CLI_PWR                   1
#define EN_CLI_TFTP                  1
#define EN_CLI_SSH                   1
#define EN_CLI_KSVC                  1
#define EN_CLI_SHA256                1
#define EN_CLI_SHA512                1
#define EN_CLI_X25519                1
#define EN_CLI_ED25519               1
#define EN_CLI_ECDSA_P256            0
#define EN_CLI_CALLTO                1
#define EN_CLI_FAT                   1
#endif

/* ===========================================================================
 * SYNCHRONIZATION PRIMITIVES CONFIGURATION
 * =========================================================================== */
#define EN_EVENT                     1
#define EN_NOTIFY                    1

/* ===========================================================================
 * SYSTEM CONFIGURATION
 * =========================================================================== */
#define POOL_QUEUE_SIZE              8

/* ===========================================================================
 * EVAL_ MACROS (generated: every symbol is already defined above)
 * =========================================================================== */
#if ( ENABLE_ITM )
#define EVAL_ITM(...)   __VA_ARGS__
#else
#define EVAL_ITM(...)
#endif
#if ( EN_USB )
#define EVAL_USB(...)   __VA_ARGS__
#else
#define EVAL_USB(...)
#endif
#if ( EN_USB_CDC && STDOUT_ON_USB_CDC && STDIN_ON_USB_CDC )
#define EVAL_USB_CDC_STD(...)   __VA_ARGS__
#else
#define EVAL_USB_CDC_STD(...)
#endif
#if ( ENABLE_STDOUT )
#define EVAL_STDOUT(...)   __VA_ARGS__
#else
#define EVAL_STDOUT(...)
#endif
#if ( EN_LED )
#define EVAL_LED(...)   __VA_ARGS__
#else
#define EVAL_LED(...)
#endif
#if ( ENABLE_CLI )
#define EVAL_CLI(...)   __VA_ARGS__
#else
#define EVAL_CLI(...)
#endif
#if ( EN_CLI_CAPTURE && ENABLE_CLI )
#define EVAL_CLI_CAPTURE(...)   __VA_ARGS__
#else
#define EVAL_CLI_CAPTURE(...)
#endif
#if ( EN_CLI_WDT )
#define EVAL_CLI_WDT(...)   __VA_ARGS__
#else
#define EVAL_CLI_WDT(...)
#endif
#if ( EN_CLI_CONFIG )
#define EVAL_CLI_CONFIG(...)   __VA_ARGS__
#else
#define EVAL_CLI_CONFIG(...)
#endif
#if ( EN_CLI_SSH && EN_SSH_TEST )
#define EVAL_CLI_SSH(...)   __VA_ARGS__
#else
#define EVAL_CLI_SSH(...)
#endif
#if ( EN_CLI_CALLTO && ENABLE_CLI )
#define EVAL_CLI_CALLTO(...)   __VA_ARGS__
#else
#define EVAL_CLI_CALLTO(...)
#endif
#if ( ENABLE_SENSOR )
#define EVAL_SENSOR(...)   __VA_ARGS__
#else
#define EVAL_SENSOR(...)
#endif
#if ( EN_ADC )
#define EVAL_ADC(...)   __VA_ARGS__
#else
#define EVAL_ADC(...)
#endif
#if ( EN_DMA )
#define EVAL_DMA(...)   __VA_ARGS__
#else
#define EVAL_DMA(...)
#endif
#if ( EN_TAMP )
#define EVAL_TAMP(...)   __VA_ARGS__
#else
#define EVAL_TAMP(...)
#endif
#if ( EN_FMAC )
#define EVAL_FMAC(...)   __VA_ARGS__
#else
#define EVAL_FMAC(...)
#endif
#if ( EN_SAI )
#define EVAL_SAI(...)   __VA_ARGS__
#else
#define EVAL_SAI(...)
#endif
#if ( EN_SPDIFRX )
#define EVAL_SPDIFRX(...)   __VA_ARGS__
#else
#define EVAL_SPDIFRX(...)
#endif
#if ( EN_DMA2D )
#define EVAL_DMA2D(...)   __VA_ARGS__
#else
#define EVAL_DMA2D(...)
#endif
#if ( EN_JPEG )
#define EVAL_JPEG(...)   __VA_ARGS__
#else
#define EVAL_JPEG(...)
#endif
#if ( EN_SDIO )
#define EVAL_SDIO(...)   __VA_ARGS__
#else
#define EVAL_SDIO(...)
#endif
#if ( EN_RTC )
#define EVAL_RTC(...)   __VA_ARGS__
#else
#define EVAL_RTC(...)
#endif
#if ( EN_NET )
#define EVAL_NET(...)   __VA_ARGS__
#else
#define EVAL_NET(...)
#endif
#if ( EN_PACKET_TRACER )
#define EVAL_PACKET_TRACER(...)   __VA_ARGS__
#else
#define EVAL_PACKET_TRACER(...)
#endif
#if ( EN_ARP_SERVICE )
#define EVAL_ARP_SVC(...)   __VA_ARGS__
#else
#define EVAL_ARP_SVC(...)
#endif
#if ( EN_ROUTE_SERVICE )
#define EVAL_ROUTE_SVC(...)   __VA_ARGS__
#else
#define EVAL_ROUTE_SVC(...)
#endif
#if ( EN_UDP_SERVER )
#define EVAL_UDP_SRV(...)   __VA_ARGS__
#else
#define EVAL_UDP_SRV(...)
#endif
#if ( EN_UDP_CLIENT )
#define EVAL_UDP_CLI(...)   __VA_ARGS__
#else
#define EVAL_UDP_CLI(...)
#endif
#if ( EN_DHCP_SERVICE )
#define EVAL_DHCP_SVC(...)   __VA_ARGS__
#else
#define EVAL_DHCP_SVC(...)
#endif
#if ( EN_NTP_SERVICE )
#define EVAL_NTP_SVC(...)   __VA_ARGS__
#else
#define EVAL_NTP_SVC(...)
#endif
#if ( EN_DNS_SERVICE )
#define EVAL_DNS_SVC(...)   __VA_ARGS__
#else
#define EVAL_DNS_SVC(...)
#endif
#if ( EN_MDNS_SERVICE )
#define EVAL_MDNS_SVC(...)   __VA_ARGS__
#else
#define EVAL_MDNS_SVC(...)
#endif
#if ( EN_SYSLOG_SERVICE )
#define EVAL_SYSLOG_SVC(...)   __VA_ARGS__
#else
#define EVAL_SYSLOG_SVC(...)
#endif
#if ( EN_TCP_SERVICE )
#define EVAL_TCP_SVC(...)   __VA_ARGS__
#else
#define EVAL_TCP_SVC(...)
#endif
#if ( EN_TCP_TRACE )
#define EVAL_TCP_TRACE(...)   __VA_ARGS__
#else
#define EVAL_TCP_TRACE(...)
#endif
#if ( EN_MQTT_SERVICE )
#define EVAL_MQTT_SVC(...)   __VA_ARGS__
#else
#define EVAL_MQTT_SVC(...)
#endif
#if ( EN_MODBUS_SERVICE )
#define EVAL_MODBUS_SVC(...)   __VA_ARGS__
#else
#define EVAL_MODBUS_SVC(...)
#endif
#if ( EN_IPERF_SERVICE )
#define EVAL_IPERF_SVC(...)   __VA_ARGS__
#else
#define EVAL_IPERF_SVC(...)
#endif
#if ( EN_HTTP_SERVICE )
#define EVAL_HTTP_SVC(...)   __VA_ARGS__
#else
#define EVAL_HTTP_SVC(...)
#endif
#if ( EN_HTTPS_SERVICE )
#define EVAL_HTTPS_SVC(...)   __VA_ARGS__
#else
#define EVAL_HTTPS_SVC(...)
#endif
#if ( EN_CRC )
#define EVAL_CRC(...)   __VA_ARGS__
#else
#define EVAL_CRC(...)
#endif
#if ( EN_RNG )
#define EVAL_RNG(...)   __VA_ARGS__
#else
#define EVAL_RNG(...)
#endif
#if ( EN_HASH )
#define EVAL_HASH(...)   __VA_ARGS__
#else
#define EVAL_HASH(...)
#endif
#if ( EN_AES )
#define EVAL_AES(...)   __VA_ARGS__
#else
#define EVAL_AES(...)
#endif
#if ( EN_EVENT )
#define EVAL_EVENT(...)   __VA_ARGS__
#else
#define EVAL_EVENT(...)
#endif
#if ( EN_NOTIFY )
#define EVAL_NOTIFY(...)   __VA_ARGS__
#else
#define EVAL_NOTIFY(...)
#endif
#if ( EN_MESSAGE )
#define EVAL_MESSAGE(...)   __VA_ARGS__
#else
#define EVAL_MESSAGE(...)
#endif
#if ( EN_STREAM )
#define EVAL_STREAM(...)   __VA_ARGS__
#else
#define EVAL_STREAM(...)
#endif
#if ( EN_MEASURE )
#define EVAL_MEASURE(...)   __VA_ARGS__
#else
#define EVAL_MEASURE(...)
#endif
#if ( EN_LPUART && STDOUT_ON_LPUART && STDIN_ON_LPUART )
#define EVAL_LPUART_STD(...)   __VA_ARGS__
#else
#define EVAL_LPUART_STD(...)
#endif
#if ( EN_FAT_SERVICE )
#define EVAL_FAT_SVC(...)   __VA_ARGS__
#else
#define EVAL_FAT_SVC(...)
#endif
#if ( EN_IP_DEFRAG )
#define EVAL_IP_DEFRAG(...)   __VA_ARGS__
#else
#define EVAL_IP_DEFRAG(...)
#endif
#if ( EN_PWR )
#define EVAL_PWR(...)   __VA_ARGS__
#else
#define EVAL_PWR(...)
#endif
#if ( EN_USB_DBG )
#define EVAL_USB_DBG(...)   __VA_ARGS__
#else
#define EVAL_USB_DBG(...)
#endif
#if ( EN_SHA256 )
#define EVAL_SHA256(...)   __VA_ARGS__
#else
#define EVAL_SHA256(...)
#endif
#if ( EN_SHA256_TEST && EN_SHA256 )
#define EVAL_SHA256_TEST(...)   __VA_ARGS__
#else
#define EVAL_SHA256_TEST(...)
#endif
#if ( EN_SHA512_TEST && EN_SHA512 )
#define EVAL_SHA512_TEST(...)   __VA_ARGS__
#else
#define EVAL_SHA512_TEST(...)
#endif
#if ( EN_X25519_TEST && EN_X25519 )
#define EVAL_X25519_TEST(...)   __VA_ARGS__
#else
#define EVAL_X25519_TEST(...)
#endif
#if ( EN_ED25519_TEST && EN_ED25519 )
#define EVAL_ED25519_TEST(...)   __VA_ARGS__
#else
#define EVAL_ED25519_TEST(...)
#endif
#if ( EN_ECDSA_P256_TEST && EN_ECDSA_P256 )
#define EVAL_ECDSA_P256_TEST(...)   __VA_ARGS__
#else
#define EVAL_ECDSA_P256_TEST(...)
#endif
#if ( EN_AES_GCM_SW_TEST && EN_AES )
#define EVAL_AES_GCM_SW_TEST(...)   __VA_ARGS__
#else
#define EVAL_AES_GCM_SW_TEST(...)
#endif
#if ( EN_SSH_SERVICE )
#define EVAL_SSH_SVC(...)   __VA_ARGS__
#else
#define EVAL_SSH_SVC(...)
#endif
#if ( EN_TFTP_SERVICE )
#define EVAL_TFTP_SVC(...)   __VA_ARGS__
#else
#define EVAL_TFTP_SVC(...)
#endif

/* ===========================================================================
 * EVAL_ MACROS on objmap.h symbols
 * =========================================================================== */
#if ENABLE_DBG_UART
#define EVAL_DBG_UART(...)   __VA_ARGS__
#else
#define EVAL_DBG_UART(...)
#endif

/* ===========================================================================
 * CONSOLE BACKEND: STDOUT/STDIN -> sender function
 * =========================================================================== */
#if ( STDOUT_ON_UART && ENABLE_DBG_UART )
#define EVAL_STDOUT_UART(...)        __VA_ARGS__
#define STDOUT                  ca_uart_dbg_put
#else
#define EVAL_STDOUT_UART(...)
#endif

#if ( STDOUT_ON_USB_CDC && EN_USB_CDC )
#define EVAL_STDOUT_CDC(...)        __VA_ARGS__
#define STDOUT                  ca_usb_cdc_write
#else
#define EVAL_STDOUT_CDC(...)
#endif

#if ( STDOUT_ON_ITM && ENABLE_ITM )
#define EVAL_STDOUT_ITM(...)        __VA_ARGS__
#define STDOUT                  ca_itm_put
#else
#define EVAL_STDOUT_ITM(...)
#endif

#if ( STDOUT_ON_LPUART && EN_LPUART )
#define EVAL_STDOUT_LPUART(...)     __VA_ARGS__
#define STDOUT                  ca_lpuart_put
#else
#define EVAL_STDOUT_LPUART(...)
#endif

/* The physical system console is mutually exclusive: exactly one of these
 * backends may drive the global STDOUT pointer. SSH is NOT in this list - it
 * is a per-session remote shell that captures CLI output on its own channel
 * (ca_cli_exec_capture), so it coexists with the physical console. */
#if ( STDOUT_ON_UART + STDOUT_ON_USB_CDC + STDOUT_ON_ITM + STDOUT_ON_LPUART ) > 1
#error "STDOUT: choose only one console backend (UART/USB_CDC/ITM/LPUART are mutually exclusive)"
#endif

#if ( STDIN_ON_UART && ENABLE_DBG_UART )
#define STDIN               ca_cli_data_from_isr
#endif

#if ( STDIN_ON_USB_CDC && EN_USB_CDC )
#define STDIN               ca_cli_data_from_isr
#endif

#if STDIN_ON_USB_NCM
#define STDIN               ca_cli_data_from_isr
#endif

#if ( STDIN_ON_LPUART && EN_LPUART )
#define STDIN               ca_cli_data_from_isr
#endif

/* Physical console input is mutually exclusive, same rationale as STDOUT.
 * SSH feeds the CLI through ca_cli_exec_capture per line, not via STDIN. */
#if ( STDIN_ON_UART + STDIN_ON_USB_CDC + STDIN_ON_USB_NCM + STDIN_ON_LPUART ) > 1
#error "STDIN: choose only one console backend (UART/USB_CDC/USB_NCM/LPUART are mutually exclusive)"
#endif

/* ===========================================================================
 * CONSTRAINTS (mutual exclusion / dependencies)
 * =========================================================================== */
#if ENABLE_UART && EN_UART_DMA
#error "ENABLE_UART and EN_UART_DMA are mutually exclusive: choose a single UART mode"
#endif

#if EN_SPI && EN_SPI_DMA
#error "EN_SPI and EN_SPI_DMA are mutually exclusive: choose a single SPI mode"
#endif

#if ( EN_UART_DMA || EN_SPI_DMA ) && !EN_DMA
#error "EN_UART_DMA/EN_SPI_DMA require EN_DMA=1"
#endif

/* ===========================================================================
 * OUTPUT MACROS (STDOUT must already be defined above)
 * =========================================================================== */
#ifdef STDOUT

#define PRINTF(BUFF,LEN,...)        ca_printf(STDOUT,BUFF,LEN,__VA_ARGS__)
#define PUT(MSG)                    ca_put(STDOUT,MSG)
#define PRINTF_N(BUFF,LEN,...)      ca_printf_n(STDOUT,BUFF,LEN,__VA_ARGS__)
#define PUT_N(MSG)                  ca_put_n(STDOUT,MSG)

#else

#define PRINTF(BUFF,LEN,...)
#define PUT(MSG)
#define PRINTF_N(BUFF,LEN,...)
#define PUT_N(MSG)
#warning "STDOUT ?"
#endif

#endif /* OS_COMPONETS_HEADER */
