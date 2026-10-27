// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    spi.h
 * @brief   SPI driver public API
 * @note    Implementation: hw/<family>/drivers/spi_<family>.c (bare-metal CMSIS)
 *          Default pins per family (software NSS):
 *          SPI1: PA5=SCK, PA6=MISO, PA7=MOSI  (AF5)
 *          SPI2: PB13=SCK, PB14=MISO, PB15=MOSI (AF5)  [ATTN: PB14=LED on H753ZI]
 *          SPI3: PC10=SCK, PC11=MISO, PC12=MOSI  (AF6 on F4/F7/H7)
 *
 *  ALTERNATIVE mapping — ca_spi_config_t.alternate_gpio = true:
 *          SPI1 ALT: PB3=SCK, PB4=MISO, PB5=MOSI  (AF5)
 *          SPI2 ALT: PB10=SCK, PC2=MISO, PC3=MOSI (AF5)
 *          SPI3 ALT: PB3=SCK, PB4=MISO, PB5=MOSI  (AF6)
 *  Required when default pins are occupied by the board (typically
 *  Ethernet RMII on Nucleo-144: PA7=RMII_CRS_DV, PB15=RMII_TXD1).
 *  On F1xx the alternative acts via AFIO->MAPR (remap), not via AF.
 *
 *  Slave IRQ mode:
 *    - on_rx != NULL → frame_size bytes accumulated in ISR, then callback
 *    - SPI_DEF_PKT_SIZE: default frame size for the example
 *    - NSS: SSM=1 on both (slave always active, no NSS pin)
 ******************************************************************************
 */

#ifndef SPI_H
#define SPI_H

#include "def_mcu.h"
#include "os_config.h"
#include "gpio_driver.h"

#if ( EN_SPI || EN_SPI_DMA )

/* Default slave frame size (override in os_config.h if necessary) */
#ifndef SPI_DEF_PKT_SIZE
#define SPI_DEF_PKT_SIZE  128U
#endif

typedef enum {
    CA_SPI_BR_DIV2   = 0,   /* FPCLK / 2   */
    CA_SPI_BR_DIV4   = 1,   /* FPCLK / 4   */
    CA_SPI_BR_DIV8   = 2,   /* FPCLK / 8   */
    CA_SPI_BR_DIV16  = 3,   /* FPCLK / 16  */
    CA_SPI_BR_DIV32  = 4,   /* FPCLK / 32  */
    CA_SPI_BR_DIV64  = 5,   /* FPCLK / 64  */
    CA_SPI_BR_DIV128 = 6,   /* FPCLK / 128 */
    CA_SPI_BR_DIV256 = 7,   /* FPCLK / 256 */
} ca_spi_br_t;

/* Slave callback: called from ISR when frame_size bytes are received */
typedef void (*ca_spi_slave_cb_t)(u8 spi_num, const u8 *data, u16 len);

typedef struct {
    u8                spi_num;     /* 1=SPI1, 2=SPI2, 3=SPI3          */
    ca_spi_br_t       br;          /* clock prescaler (master only)    */
    u8                cpol;        /* 0=idle low,  1=idle high         */
    u8                cpha;        /* 0=1st edge,  1=2nd edge          */
    bool              master;      /* true=master, false=slave         */
    u16               frame_size;  /* bytes per frame (slave IRQ mode)  */
    ca_spi_slave_cb_t on_rx;       /* NULL=polling, non-NULL=IRQ slave */
    bool              alternate_gpio; /* true=alternative pin mapping   */
    bool              manage_cs;   /* true=driver manages CS via GPIO */
    u8                slave_tx_fill; /* slave IRQ: byte answered on MISO for each byte in, 0=none */
    ca_gpio_port_t       cs_port;     /* CS GPIO port                */
    ca_gpio_pin_t        cs_pin;      /* CS GPIO pin                  */
} ca_spi_config_t;

/* Init — configure master or slave based on cfg->master */
bool ca_spi_init(const ca_spi_config_t *cfg);

/* Stop — disable the peripheral (SPE=0) and silence its interrupt, so the
 * pins can be taken over as plain GPIO or the SPI powered down. Any transfer
 * in flight is allowed to drain first. The RCC clock and the pin alternate
 * function are left as they are: call ca_spi_init() to use the SPI again.
 * Returns false on an invalid spi_num or if a transfer would not finish. */
bool ca_spi_stop(u8 spi_num);

/* CS assert/deassert — no-op if manage_cs==false */
void ca_spi_cs_assert(u8 spi_num);
void ca_spi_cs_deassert(u8 spi_num);
void ca_spi_slave_sync(u8 spi_num);   /* slave: call at frame start (CS edge): drops a partial frame, clears overrun */

/* Longest wait for one byte in polling mode. A master finishes on its own
 * clock; a slave waits for the master's, and gives up after this. */
#ifndef CA_SPI_XFER_TIMEOUT_MS
#define CA_SPI_XFER_TIMEOUT_MS  1000U
#endif

/* Single byte full-duplex transfer (master or slave polling).
 * Returns 0 if the byte did not complete within CA_SPI_XFER_TIMEOUT_MS. */
u8   ca_spi_xfer(u8 spi_num, u8 data);

/* Buffer transfer (master or slave polling).
 * Return the bytes actually moved: < len if one timed out. */
u16  ca_spi_write_buf(u8 spi_num, const u8 *buf, u16 len);
u16  ca_spi_read_buf(u8 spi_num, u8 *buf, u16 len);

/* Call from SPI<n>_IRQHandler — accumulates bytes and invokes callback */
void ca_spi_recv_handler(u8 spi_num);

#if EN_SPI_DMA
/* ============================================================================
 * DMA MODE (EN_SPI_DMA) — master only, SYNCHRONOUS transfer
 * ============================================================================
 * SPI is synchronous: the master clocks exactly len bytes, so the transfer
 * size is known a priori (on IP v3 H5/H7/U5 it becomes TSIZE).
 * ca_spi_dma_xfer arms the two DMA channels (RX P2M + TX M2P), starts the transfer
 * and blocks the task until completion: no active waiting for bytes.
 * CS: as in polling, managed by the caller (ca_spi_cs_assert/deassert).
 * NOTE H7: buffers must reside in RAM D2 (no DTCM). */

/* Init DMA — like ca_spi_init (cfg->master mandatory) + allocate and bind
 * the peripheral's RX/TX DMA channels. Requires ca_dma_init() already called. */
bool ca_spi_init_dma(const ca_spi_config_t *cfg);

/* Blocking full-duplex transfer: clocks len bytes.
 * tx==NULL → sends dummy 0xFF (uses rx as source, pre-filled);
 * rx==NULL → TX-only, received bytes are discarded.
 * Returns len if ok, 0 on error or if the DMA does not finish within
 * 10 ms + 0.5 ms per byte (any SPI clock above 16 kbit/s fits). */
u16 ca_spi_dma_xfer(u8 spi_num, const u8 *tx, u8 *rx, u16 len);

/* Convenience wrappers */
u16 ca_spi_dma_write(u8 spi_num, const u8 *buf, u16 len);
u16 ca_spi_dma_read(u8 spi_num, u8 *buf, u16 len);
#endif /* EN_SPI_DMA */

#else  /* EN_SPI == 0 && EN_SPI_DMA == 0 */

/* SPI disabled: keep only the ISR handler declared (no-op stub in spi.c) so the
 * weak SPIx_IRQHandler in handlers.c still compiles. Public SPI API unavailable.
 * Mirrors the i2c.h #else convention. */
void ca_spi_recv_handler(u8 spi_num);

#endif /* EN_SPI || EN_SPI_DMA */
#endif /* SPI_H */
