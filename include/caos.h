// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : caos.h
 * @brief          : Main header of the caOS operating system
 * @author         : Angelo Coppi
 ******************************************************************************
 * @details
 * Main header that includes all modules of the caOS operating system.
 * Provides access to all operating system functionalities in a single
 * inclusion. Includes modules for scheduling, synchronization, memory
 * management, logging, hardware drivers, and much more.
 *
 * Typical sequence:
 * 1) Inclusion of caos.h -> access to all operating system APIs
 ******************************************************************************
 */

#ifndef CAOS_HEADERS_INC
#define CAOS_HEADERS_INC


#include "atomic.h"
#include "bus.h"
#include "clock.h"
#include "common.h"
#include "crc32.h"
#include "endian.h"
#include "event.h"
#include "handler.h"
#include "itm.h"
#include "stdout.h"
#include "memaux.h"
#include "memory.h"
#include "message.h"
#include "measure.h"
#include "mpu.h"
#include "notify.h"
#include "queue.h"
#include "safeguard.h"
#include "scheduler.h"
#include "stream.h"
#include "sync.h"
#include "systick.h"
#include "sw_timer.h"
#include "utils.h"
#include "cli.h"
#include "callto.h"
#include "sha256.h"
#include "kdf.h"
#include "sha512.h"
#include "x25519.h"
#include "ed25519.h"
#include "ecdsa_p256.h"
#include "keys_store.h"
#include "aes_channel_service.h"
#include "secure_pipe_service.h"
#include "keystore_service.h"

// drivers
#include "gpio_driver.h"
#include "led.h"
#include "uart.h"
#include "wdt.h"
#include "bkp.h"
#include "cordic.h"
#include "fmac.h"
#include "sai.h"
#include "spdifrx.h"
#include "dma2d.h"
#include "jpeg.h"
#include "opamp.h"
#include "temp_sensor.h"
#include "usb.h"
// peripheral drivers
#include "dma.h"
#include "exti.h"
#include "spi.h"
#include "i2c.h"
#include "adc.h"
#include "dac.h"
#include "flash.h"
#include "qspi.h"
#include "sdio.h"
#include "fat_service.h"
#include "timer.h"
#include "lptim.h"
#include "lpuart.h"
#include "rtc.h"
#include "pwr.h"
#include "can.h"
#include "eth.h"
#include "net_buf.h"
#include "net_queue.h"
#include "net.h"
#include "eth_iface.h"
#include "net_utils.h"
#include "arp_service.h"
#include "ip_defrag.h"
#include "route_service.h"
#include "udp_server.h"
#include "udp_client.h"
#include "dhcp_service.h"
#include "ntp_service.h"
#include "dns_service.h"
#include "mdns_service.h"
#include "syslog_service.h"
#include "tcp_service.h"
#include "mqtt_service.h"
#include "modbus_service.h"
#include "iperf_service.h"
#include "http_service.h"
#include "tftp_service.h"
#include "boot_image.h"
#include "ssh_service.h"
#include "packet_tracer_service.h"
#include "usb_mgmt.h"
#include "usb_custom.h"
#include "usb_cdc.h"
#include "usb_msc.h"
#include "usb_ncm.h"
#include "usb_dfu.h"
#include "ncm_iface.h"
#include "rng.h"
#include "hash.h"
#include "aes.h"
#include "tls.h"
#include "https_service.h"
#include "pool.h"
#include "block_pool.h"
// hw glue interface (last: uses types from all driver headers above)
#include "ca_glue_hw.h"
#include "caos_info.h"

/* user program create task from super user */
extern void ca_os_main(void);
/* init hardware and software */
extern void ca_os_configure(void);
/* let's go! */
extern void ca_os_run(bool active);
/* caOS stops for good: SysTick, NVIC, MPU off, the CPU back in thread mode on
   MSP, privileged, interrupts masked, clocks back to the reset tree. image:
   address of a vector table to start (VTOR, MSP, reset handler); 0 = stay in
   ca_os_dead(). Does not return. */
extern void ca_os_end(u32 image);
/* banner os */
extern void ca_os_banner(void);
/* direct banner printing (no task): callable from task or ISR context,
   useful for reprinting it when a USB CDC terminal opens (DTR on). */
extern u32 ca_system_banner(void *arg);
#endif