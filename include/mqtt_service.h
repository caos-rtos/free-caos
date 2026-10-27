// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    mqtt_service.h
 * @brief   caOS MQTT client service — v3.1.1, QoS 0, non-blocking
 *
 * @note    Architecture:
 *
 *            Application / ca_os_configure:
 *              ca_mqtt_service_init(broker_ip, port, local_ip, local_mac,
 *                                   client_id, on_connected, on_message,
 *                                   on_disconnected)
 *
 *            Application / ca_os_main:
 *              ca_tcp_service_start()  ← required by TCP layer
 *
 *            net task (after ca_net_proto_init):
 *              ca_mqtt_service_net_ready()   ← triggers TCP connect
 *
 *            TCP layer (async callbacks, no dedicated task):
 *              TCP ESTABLISHED → MQTT CONNECT sent automatically
 *              CONNACK OK      → on_connected() fired
 *              PUBLISH rx      → on_message(topic, tlen, payload, plen) fired
 *              TCP closed      → on_disconnected() fired
 *
 *            Application keeps the connection alive:
 *              call ca_mqtt_ping() periodically (every < MQTT_KEEPALIVE_SEC)
 *
 *          Configuration (os_config.h under EN_MQTT_SERVICE):
 *            MQTT_RX_BUF_SIZE    — max payload size for RX packets (default 256)
 *            MQTT_KEEPALIVE_SEC  — keepalive value advertised to broker (default 60)
 ******************************************************************************
 */

#ifndef MQTT_SERVICE_H_
#define MQTT_SERVICE_H_

#include "common.h"
#include "os_config.h"
#include "net_cfg.h"

#if EN_MQTT_SERVICE && EN_NET && EN_TCP_SERVICE

/* ----------------------------------------------------------------------- */
/* Callbacks                                                                */
/* ----------------------------------------------------------------------- */

/** Called when MQTT CONNACK is received with return code 0 (accepted). */
typedef void (*ca_mqtt_connected_cb_t)(void);

/**
 * Called when a PUBLISH packet is received.
 * topic and payload are NOT null-terminated — use the length parameters.
 * Pointers are valid only for the duration of the callback.
 */
typedef void (*ca_mqtt_message_cb_t)(const char *topic,  u16 topic_len,
                                     const char *payload, u16 payload_len);

/** Called when the TCP connection is closed or lost. */
typedef void (*ca_mqtt_disconnected_cb_t)(void);

/* ----------------------------------------------------------------------- */
/* API                                                                      */
/* ----------------------------------------------------------------------- */

/**
 * Initialise MQTT service state.
 * Call from ca_os_configure() BEFORE ca_mqtt_service_net_ready().
 *
 * @param broker_ip      IPv4 of the MQTT broker
 * @param broker_port    TCP port (typically 1883)
 * @param local_ip       local IPv4 address
 * @param local_mac      local MAC address
 * @param client_id      MQTT client identifier (null-terminated, max 23 chars)
 * @param on_connected   fired on CONNACK accepted (may be NULL)
 * @param on_message     fired on incoming PUBLISH (may be NULL)
 * @param on_disconnected fired on TCP close/error (may be NULL)
 */
void ca_mqtt_service_init(ca_ipv4_t                   broker_ip,
                          u16                      broker_port,
                          ca_ipv4_t                   local_ip,
                          ca_mac_t                    local_mac,
                          const char              *client_id,
                          ca_mqtt_connected_cb_t   on_connected,
                          ca_mqtt_message_cb_t     on_message,
                          ca_mqtt_disconnected_cb_t on_disconnected);

/**
 * Trigger the TCP connect to the broker.
 * Call from net task AFTER ca_net_proto_init() and local IP is valid.
 * Returns false if TCP connect slot is unavailable.
 */
bool ca_mqtt_service_net_ready(void);

/**
 * Publish a message (QoS 0, no acknowledgement).
 * Returns false if not MQTT-connected or TCP send fails.
 *
 * @param topic    null-terminated topic string
 * @param payload  raw payload bytes
 * @param len      payload length in bytes
 */
bool ca_mqtt_publish(const char *topic, const u8 *payload, u16 len);

/**
 * Subscribe to a topic (QoS 0).
 * Returns false if not MQTT-connected.
 *
 * @param topic  null-terminated topic string (wildcards + and # supported)
 */
bool ca_mqtt_subscribe(const char *topic);

/**
 * Send a PINGREQ to the broker.
 * Call periodically (every < MQTT_KEEPALIVE_SEC seconds) to keep
 * the connection alive. Application is responsible for the timing.
 */
void ca_mqtt_ping(void);

/** Returns true if the MQTT CONNACK has been received and the session is active. */
bool ca_mqtt_is_connected(void);

/** Gracefully close the TCP connection and reset service state. */
void ca_mqtt_disconnect(void);

#endif /* EN_MQTT_SERVICE && EN_NET && EN_TCP_SERVICE */
#endif /* MQTT_SERVICE_H_ */
