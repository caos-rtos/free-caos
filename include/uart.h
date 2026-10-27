// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com



/**
 ******************************************************************************
 * @file           : uart.h
 * @brief          : Common UART driver interface for CAOS
 * @author         : Angelo Coppi
 ******************************************************************************
 */

#ifndef UART_H
#define UART_H

#include "caos.h"

/* =============================================================================
 * UART BAUD RATE ENUMERATION
 * ============================================================================= */

/**
 * @brief Enumeration of standard baud rates for UART
 * @details The values represent commonly used baud rates,
 *          up to the maximum supported by STM32 microcontrollers
 */
typedef enum {
    CA_UART_BAUD_9600    = 9600,      /*!< 9600 bps */
    CA_UART_BAUD_19200   = 19200,     /*!< 19200 bps */
    CA_UART_BAUD_38400   = 38400,     /*!< 38400 bps */
    CA_UART_BAUD_57600   = 57600,     /*!< 57600 bps */
    CA_UART_BAUD_115200  = 115200,    /*!< 115200 bps */
    CA_UART_BAUD_230400  = 230400,    /*!< 230400 bps */
    CA_UART_BAUD_460800  = 460800,    /*!< 460800 bps */
    CA_UART_BAUD_921600  = 921600,    /*!< 921600 bps */
    CA_UART_BAUD_1000000 = 1000000,   /*!< 1 Mbps */
    CA_UART_BAUD_2000000 = 2000000,   /*!< 2 Mbps */
    CA_UART_BAUD_3000000 = 3000000,   /*!< 3 Mbps */
    CA_UART_BAUD_4000000 = 4000000,   /*!< 4 Mbps - Theoretical maximum for some STM32 */
    CA_UART_BAUD_4500000 = 4500000,   /*!< 4.5 Mbps - Practical maximum for some STM32 */
    CA_UART_BAUD_5000000 = 5000000,   /*!< 5 Mbps - Theoretical maximum for some high-performance STM32 */
} ca_uart_baud_rate_t;

/* Byte reception callback on generic UART */
typedef void (*ca_uart_rx_cb_t)(u32 rx_data);

/* Pin map for the debug console UART. A board whose VCP is not on the driver's
 * default pins overrides it in its own objmap.h; every board so far uses 0. */
#ifndef UART_DBG_PINMAP
#define UART_DBG_PINMAP  0U
#endif

#if  ( ENABLE_DBG_UART || ENABLE_UART || EN_UART_DMA )

/* ============================================================================
 * UART DRIVER PROTOTYPES
 * ========================================================================== */

/**
 * @brief Configures GPIO pins for the specified UART
 * @param uart_num UART port number
 * @return true if successful, false otherwise
 */
bool ca_uart_configure_gpio(u32 uart_num);

/**
 * @brief Initializes the UART port for debug
 * @param uart_num UART port number (e.g., 1, 2, 3, etc.)
 * @param baudrate Communication speed (e.g., 9600, 115200, etc.)
 * @return true if initialization is successful, false otherwise
 */
bool ca_uart_init(u32 uart_num, u32 baudrate, ca_uart_rx_cb_t on_rx, u8 alternate);

/* ---------------------------------------------------------------------------
 * `alternate` (last argument of ca_uart_init / ca_uart_init_dma): which PIN
 * PAIR carries the UART. 0 is the mapping the driver has always programmed, so
 * passing 0 keeps the old behaviour exactly.
 *
 * It is an argument of the init and NOT a separate call on purpose: the init
 * programs the pins itself, so a "set it afterwards" API would first drive the
 * DEFAULT pair and only then move away from it - and on a NUCLEO-144 the
 * USART2 default PA2/PA3 is the ST-LINK VCP, i.e. pins nobody asked the driver
 * to touch.
 *
 * It exists because the reference manual says which pins CAN carry a UART
 * while the board's user manual says which ones are REACHABLE: on the
 * NUCLEO-F446ZE, PA2/PA3 never reach a connector, so USART2 is only usable
 * there through alternate 1 (PD5/PD6, the pins UM1974 labels USART_B).
 *
 * An alternate a family does not declare falls back to 0.
 *
 *   F4xx  USART2: 0 = PA2/PA3 (AF7)   1 = PD5/PD6 (AF7)   [DS10693 Table 11]
 * ------------------------------------------------------------------------- */


/**
 * @brief Reprograms the baudrate of active UARTs after a clock change
 * @details To be called after a change in HCLK/PCLK (ca_pwr_set_freq)
 */
void ca_uart_retune(void);

/**
 * @brief Sends a message via UART
 * @param uart_num UART port number
 * @param message Pointer to the message to send
 * @return Number of bytes sent
 */
u16 ca_uart_put(u32 uart_num, p_str message);

/**
 * @brief Sender-compatible wrapper for UART_DBG_NUM (used as STDOUT)
 * @param message Pointer to the message to send
 * @return Number of bytes sent
 */
u16 ca_uart_dbg_put(p_str message);

/**
 * @brief UART data reception handler (to be called in ISR or appropriate context)
 * @param uart_num UART port number
 */
void ca_uart_recv_handler(u32 uart_num);

#if EN_UART_DMA
/**
 * @brief Initializes the UART in DMA mode (header-then-payload protocol):
 *        starts in normal mode with on_rx per byte (header phase, ISR context);
 *        TX via DMA one-shot (ca_uart_put unchanged)
 * @param uart_num UART port number
 * @param baudrate Communication speed
 * @param on_rx    Callback for each byte received in header phase (ISR context)
 * @return true if initialization is successful
 */
bool ca_uart_init_dma(u32 uart_num, u32 baudrate, ca_uart_rx_cb_t on_rx, u8 alternate);

/**
 * @brief Arms the one-shot DMA reception of len bytes into the caller's buffer.
 *        Callable from on_rx (ISR): RXNEIE off, the DMA fills buf without
 *        interrupts per byte. At TC, the driver returns to header mode and
 *        wakes the task in ca_uart_dma_wait().
 * @note  H7: buf must be in RAM D2 (no DTCM)
 * @return true if the transfer has been armed
 */
bool ca_uart_dma_recv(u32 uart_num, u8 *buf, u16 len);

/**
 * @brief Blocks the current task until the DMA reception TC (end of frame),
 *        at most timeout_ms (CA_WAIT_FOREVER: no deadline).
 *        Counting semaphore: if the TC has already occurred, it returns immediately.
 * @return false if the frame did not complete in time: call ca_uart_dma_abort()
 *         to return to header mode.
 */
bool ca_uart_dma_wait(u32 uart_num, u32 timeout_ms);

/**
 * @brief Missing bytes of the ongoing DMA reception (0 = completed).
 *        Useful for diagnosis/timeout on truncated frames.
 */
u32 ca_uart_dma_remaining(u32 uart_num);

/**
 * @brief Cancels the ongoing DMA reception and returns to header mode.
 *        The TC will not arrive: do not call ca_uart_dma_wait() after the abort.
 */
void ca_uart_dma_abort(u32 uart_num);

/**
 * @brief Transmits len bytes via DMA one-shot (M2P). The task blocks on the
 *        channel semaphore until the TC: only from task, NEVER from ISR.
 *        ca_uart_put() internally uses this function (130B string limit);
 *        for protocol frames, use this directly.
 * @note  H7: buf must be in RAM D2 (no DTCM)
 * @return Bytes transmitted (0 on error, or if the DMA did not finish within
 *         the line time of len bytes plus 10 ms)
 */
u16 ca_uart_dma_send(u32 uart_num, const u8 *buf, u16 len);
#endif

#else

/* Empty macros when debug UART is disabled */
#define ca_uart_init(...)
#define ca_uart_init_dma(...)
#define ca_uart_dma_recv(...)
#define ca_uart_dma_wait(...)
#define ca_uart_dma_remaining(...)
#define ca_uart_dma_abort(...)
#define ca_uart_dma_send(...)
#define ca_uart_put(...)
#define ca_uart_configure_gpio(...)
#define ca_uart_retune(...)
void ca_uart_recv_handler(u32 uart_num);
#endif

#endif /* UART_H */