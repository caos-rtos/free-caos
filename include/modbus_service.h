// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    modbus_service.h
 * @brief   caOS Modbus TCP client service
 *
 * @note    Protocol: Modbus over TCP (port 502), MBAP + PDU framing.
 *
 *          Supported function codes:
 *            FC01  Read Coils
 *            FC03  Read Holding Registers
 *            FC06  Write Single Register
 *            FC16  Write Multiple Registers
 *
 *          Architecture:
 *            - Blocking API: caller blocks until response or timeout
 *            - Max 1 transaction in flight (Modbus is request/response)
 *            - No dedicated task — uses TCP callback for RX assembly
 *            - TCP connect is asynchronous; blocking calls wait for tcp_up
 *
 *          Usage:
 *            ca_os_configure():
 *              ca_modbus_service_init(server_ip, port, local_ip, mac, unit_id)
 *
 *            after DHCP bound (net task):
 *              ca_modbus_service_net_ready()   -- triggers TCP connect
 *
 *            application task:
 *              ca_modbus_read_holding_regs(addr, count, out_buf)
 *              ca_modbus_write_single_reg(addr, value)
 *              etc.
 *
 *          Configuration (os_config.h):
 *            MODBUS_TIMEOUT_MS   — response timeout (default 1000 ms)
 *            MODBUS_RX_BUF_SIZE  — RX buffer (default 270 bytes)
 ******************************************************************************
 */

#ifndef MODBUS_SERVICE_H_
#define MODBUS_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_MODBUS_SERVICE && EN_NET && EN_TCP_SERVICE

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise Modbus TCP client state.
 * Call from ca_os_configure() before ca_modbus_service_net_ready().
 *
 * @param server_ip    IPv4 of the Modbus TCP server (slave device)
 * @param server_port  TCP port (typically 502)
 * @param local_ip     local IPv4 address
 * @param local_mac    local MAC address
 * @param unit_id      Modbus unit identifier (1–247; 0xFF = broadcast)
 */
void ca_modbus_service_init(ca_ipv4_t server_ip,
                             u16    server_port,
                             ca_ipv4_t local_ip,
                             ca_mac_t  local_mac,
                             u8     unit_id);

/**
 * Trigger TCP connect to the server.
 * Call after DHCP is bound and local IP is valid.
 * Returns false if TCP connect slot is unavailable.
 */
bool ca_modbus_service_net_ready(void);

/** Returns true if TCP connection to server is established. */
bool ca_modbus_is_connected(void);

/** Close TCP connection and reset service state. */
void ca_modbus_disconnect(void);

/* ----------------------------------------------------------------------- */
/* FC01 — Read Coils                                                        */
/* ----------------------------------------------------------------------- */

/**
 * Read coils (FC01).
 * Each coil is packed into bits of out[]; bit 0 of out[0] = coil at addr.
 *
 * @param addr   starting coil address (0-based)
 * @param count  number of coils to read (1–2000)
 * @param out    output buffer — must be at least ceil(count/8) bytes
 * @return true on success, false on timeout or error response
 */
bool ca_modbus_read_coils(u16 addr, u16 count, u8 *out);

/* ----------------------------------------------------------------------- */
/* FC03 — Read Holding Registers                                            */
/* ----------------------------------------------------------------------- */

/**
 * Read holding registers (FC03).
 *
 * @param addr   starting register address (0-based)
 * @param count  number of registers to read (1–125)
 * @param out    output buffer — must be at least count elements
 * @return true on success, false on timeout or error response
 */
bool ca_modbus_read_holding_regs(u16 addr, u16 count, u16 *out);

/* ----------------------------------------------------------------------- */
/* FC06 — Write Single Register                                             */
/* ----------------------------------------------------------------------- */

/**
 * Write a single holding register (FC06).
 *
 * @param addr   register address (0-based)
 * @param value  16-bit value to write
 * @return true on success, false on timeout or error response
 */
bool ca_modbus_write_single_reg(u16 addr, u16 value);

/* ----------------------------------------------------------------------- */
/* FC16 — Write Multiple Registers                                          */
/* ----------------------------------------------------------------------- */

/**
 * Write multiple holding registers (FC16).
 *
 * @param addr   starting register address (0-based)
 * @param values array of 16-bit values to write
 * @param count  number of registers (1–123)
 * @return true on success, false on timeout or error response
 */
bool ca_modbus_write_multiple_regs(u16 addr, const u16 *values, u16 count);

#endif /* EN_MODBUS_SERVICE && EN_NET && EN_TCP_SERVICE */
#endif /* MODBUS_SERVICE_H_ */
