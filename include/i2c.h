// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    i2c.h
 * @brief   caOS public API for I2C driver via libopencm3
 * @note    Use def_mcu.h (not common.h) to be safe in libopencm3 context.
 ******************************************************************************
 */

#ifndef CAOS_I2C_H
#define CAOS_I2C_H

#include "def_mcu.h"
#include "os_config.h"

/* irq_req value: enables IRQ + NVIC when passed to ca_i2c_init */
#define I2C_EN_IRQ  ((u16)0x0102U)

#if EN_I2C

typedef enum __attribute__ ((packed)) {
    CA_I2C_SPEED_STANDARD = 0,   /* 100 kHz */
    CA_I2C_SPEED_FAST     = 1,   /* 400 kHz */
} ca_i2c_speed_t;

typedef struct {
    u8             i2c_num;   /* 1, 2, 3 */
    ca_i2c_speed_t speed;
    u16            irq_req;   /* I2C_EN_IRQ to enable EV IRQ+NVIC, 0 = polling only */
    u8             alternate; /* pin map: 0 = default, 1..N = alternate (see below) */
} ca_i2c_config_t;

/* ---------------------------------------------------------------------------
 * alternate: which PINS carry the bus, not which AF number.
 *
 * 0 is the mapping the driver has always used, so a config that leaves the
 * field zeroed keeps working unchanged. 1 and up are the further pin pairs a
 * family brings out, declared in hw/<family>/drivers/i2c_<family>.c.
 *
 * It exists because the reference manual says which pins CAN carry I2C while
 * the board's user manual says which ones are REACHABLE, and the two disagree
 * often enough to matter: on the NUCLEO-F446ZE the default I2C1 pair PB6/PB7
 * puts SDA on the blue LD2, and PB11 (I2C2 SDA) reaches no connector at all.
 *
 * An alternate a family does not define falls back to 0 rather than leaving
 * the pins unconfigured - a wrong-but-known mapping is easier to diagnose
 * than a silent no-op.
 *
 *   F4xx  I2C1: 0 = PB6/PB7    1 = PB8/PB9   (NUCLEO-144: CN7-2 / CN7-4)
 *         I2C2: 0 = PB10/PB11  1 = PF1/PF0   (NUCLEO-144: CN9-19 / CN9-21)
 * ------------------------------------------------------------------------- */

/* Slave RX callback — called from ISR when master completes a write transfer */
typedef void (*ca_i2c_slave_rx_cb)(u8 num, const u8 *buf, u8 len);

/* Slave context — shared between i2c.c and HW drivers (g_i2c_slave_ctx[num-1]) */
typedef struct {
    u8                 addr;
    u8                 rx_buf[64];
    u8                 rx_idx;
    u8                 tx_buf[64];
    u8                 tx_len;
    u8                 tx_idx;
    bool               is_slave;
    ca_i2c_slave_rx_cb rx_cb;
} ca_i2c_slave_ctx_t;

extern ca_i2c_slave_ctx_t g_i2c_slave_ctx[4];   /* indexed by [i2c_num - 1] */

bool ca_i2c_init(const ca_i2c_config_t *cfg);

/**
 * @brief Clears a bus a slave holds stuck (SDA low after a reset mid-transfer):
 *        up to 9 SCL pulses as GPIO, a STOP, then the bus re-initialised with
 *        the pins, speed and IRQ setup given to ca_i2c_init().
 *        ca_i2c_init() already runs it once; call it again after a timeout.
 * @return true if the bus is idle afterwards (false: slave dead, short, or
 *         num never initialised)
 */
bool ca_i2c_bus_recovery(u8 num);

/* write: addr (7-bit), buf, len */
bool ca_i2c_write(u8 i2c_num, u8 addr, const u8 *buf, u16 len);

/* read: addr (7-bit), buf, len */
bool ca_i2c_read(u8 i2c_num, u8 addr, u8 *buf, u16 len);

/* write then read in single transaction (reg read pattern) */
bool ca_i2c_write_read(u8 i2c_num, u8 addr,
                       const u8 *wbuf, u16 wlen,
                       u8 *rbuf, u16 rlen);

/* Initialize I2C peripheral in slave mode (IRQ-driven, 7-bit address).
 * alternate: same meaning as in ca_i2c_config_t, 0 = default pin map. */
bool ca_i2c_slave_init(u8 i2c_num, u8 slave_addr, ca_i2c_slave_rx_cb cb,
                       u8 alternate);

/* Prepare TX buffer to respond to next master read (call from slave RX callback) */
void ca_i2c_slave_set_tx(u8 i2c_num, const u8 *buf, u8 len);

/* called from I2Cx_EV_IRQHandler */
void ca_i2c_recv_handler(u32 i2c_num);

/* called from I2Cx_ER_IRQHandler */
void ca_i2c_err_handler(u32 i2c_num);

#else  /* EN_I2C == 0 */

#define ca_i2c_init(...)
#define ca_i2c_write(...)
#define ca_i2c_read(...)
#define ca_i2c_write_read(...)
#define ca_i2c_slave_init(...)
#define ca_i2c_slave_set_tx(...)
/* called from I2Cx_EV_IRQHandler */
void ca_i2c_recv_handler(u32 i2c_num);

/* called from I2Cx_ER_IRQHandler */
void ca_i2c_err_handler(u32 i2c_num);
#endif /* EN_I2C */

#endif /* CAOS_I2C_H */
