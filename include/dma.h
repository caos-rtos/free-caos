// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    dma.h
 * @brief   DMA driver public API — generic framework
 * @note    HW implementation: hw/<family>/drivers/dma_<family>.c
 *
 *  CHANNEL ALLOCATION:
 *    ca_dma_init()                — initialize driver (semaphores, state)
 *    ca_dma_get_channel_count()   — number of available HW channels
 *    ca_dma_alloc(out)            — allocate first free channel
 *    ca_dma_release(ch)           — release channel
 *
 *  TRANSFER:
 *    ca_dma_copy_inc(ch,src,dst,size)        — M→M both pointers increment
 *    ca_dma_copy_target(ch,mem,periph,size,dir) — M↔P one fixed endpoint
 *
 *  COMPLETION (scheduler-aware semaphore):
 *    ca_dma_wait(ch, timeout_ms)  — block task until TC or the timeout
 *    ca_dma_isr_complete(ctrl,stream) — call from each channel's ISR
 *
 *  BITMASK:
 *    in_use_dma — bit N=1 → flat channel N in use
 *
 *  Inaccessible memory per channel: validated with CA_SAFEGUARD in dma_<family>.c
 ******************************************************************************
 */

#ifndef DMA_H
#define DMA_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_DMA

/* Maximum number of flat channels (H7xx: DMA1+DMA2+BDMA = 24) */
#define DMA_MAX_CHANNELS   24U

/* ---- Channel identifier: {controller, stream/channel} ---- */
typedef struct {
    u8 controller;  /* 1=DMA1, 2=DMA2, 3=BDMA (H7) / LPDMA (H5) */
    u8 stream;      /* stream/channel index (0-based) */
} ca_dma_ch_t;

#define DMA_CH_INVALID  ((ca_dma_ch_t){0xFFU, 0xFFU})

/* No associated request line (default: SW request / M2M) */
#define DMA_REQ_NONE    0xFFFFU

/* ---- Transfer direction (for ca_dma_copy_target) ---- */
typedef enum {
    CA_DMA_DIR_M2P = 0,  /* memory → peripheral (TX) */
    CA_DMA_DIR_P2M = 1,  /* peripheral → memory  (RX) */
} ca_dma_dir_t;

/* ---- DMA element width ---- */
typedef enum {
    CA_DMA_WIDTH_8  = 0,  /* 8-bit  — size in bytes      */
    CA_DMA_WIDTH_16 = 1,  /* 16-bit — size in halfwords   */
    CA_DMA_WIDTH_32 = 2,  /* 32-bit — size in words       */
} ca_dma_width_t;

/* =============================================================================
 * PUBLIC API
 * ============================================================================= */

/* Initialize DMA driver (semaphores + bitmask). Call first. */
void ca_dma_init(void);

/* Returns the total number of DMA channels available on this MCU. */
u8 ca_dma_get_channel_count(void);

/* Find and allocate the first free channel.
 * Returns the flat index (0..N-1) or -1 if all are occupied.
 * Fills *out with the descriptor {controller, stream}. */
i16 ca_dma_free_channel(ca_dma_ch_t *out);

/* Allocate a specific channel (flat index).
 * Returns index if free and allocated, -1 if already in use or out of range.
 * Fills *out with the descriptor {controller, stream}. */
/**
 * @brief Reserve a free channel that can perform memory-to-memory transfers
 * @param out receives the channel descriptor
 * @return flat index, or -1 if none is free
 *
 * Not every controller can do M2M: on F4 and F7 only DMA2 can (RM0390 9.3.7,
 * "Only the DMA2 controller is able to perform memory-to-memory transfers"),
 * and a M2M transfer armed on DMA1 never starts - the transfer-complete
 * interrupt never fires and ca_dma_wait() blocks forever. Use this instead of
 * ca_dma_free_channel() whenever the destination is memory; where every
 * controller can do M2M it behaves exactly like ca_dma_free_channel().
 */
i16 ca_dma_free_channel_m2m(ca_dma_ch_t *out);

i16 ca_dma_free_channel_index(i16 index, ca_dma_ch_t *out);

/* Release a previously allocated channel. */
void ca_dma_release(ca_dma_ch_t ch);

/* Associate an HW request line to the channel — call AFTER allocation and
 * BEFORE copy_target/start_circular. The request value is family-specific
 * (peripheral request number, e.g., USART1_RX):
 *   F1xx : ignored — channel↔peripheral mapping fixed in HW: allocate the
 *          correct channel with ca_dma_free_channel_index()
 *   F4/F7: channel select 0..7 (CHSEL in SxCR)
 *   H7xx : request id DMAMUX1 (DMA1/2) / DMAMUX2 (BDMA)
 *   H5/U5: REQSEL GPDMA (CTR2)
 * DMA_REQ_NONE restores the default (SW request / M2M).
 * ca_dma_release() clears the associated request. */
bool ca_dma_set_request(ca_dma_ch_t ch, u16 request);

/* Start M→M transfer (both pointers increment).
 * size = number of elements (bytes / halfwords / words depending on width).
 * Non-blocking: use ca_dma_wait() to wait for completion. */
bool ca_dma_copy_inc(ca_dma_ch_t ch, const void *src, void *dst,
                     u32 size, ca_dma_width_t width);

/* Start M↔P transfer: memory increments, periph is fixed.
 * dir=CA_DMA_DIR_M2P: mem→periph (TX)
 * dir=CA_DMA_DIR_P2M: periph→mem (RX)
 * size = number of elements (bytes / halfwords / words depending on width).
 * Non-blocking: use ca_dma_wait() to wait for completion. */
bool ca_dma_copy_target(ca_dma_ch_t ch,
                        void *mem, volatile void *periph,
                        u32 size, ca_dma_dir_t dir, ca_dma_width_t width);

/* Block the current task until transfer completion, at most timeout_ms
 * (CA_NO_WAIT_TIME / CA_WAIT_FOREVER as for every blocking call).
 * false = no TC in time: the stream is left armed - call ca_dma_abort() to
 * stop it (or the next ca_dma_copy_target / ca_dma_copy_inc stops it). */
bool ca_dma_wait(ca_dma_ch_t ch, u32 timeout_ms);

/* Stop the transfer in progress on the channel, if any, without a completion:
 * no TC reaches ca_dma_wait() afterwards, the channel stays allocated with its
 * request routing, ready for the next ca_dma_copy_*. For a transfer given up
 * on a timeout (peer silent, DMA not moving). */
void ca_dma_abort(ca_dma_ch_t ch);

/* Remaining elements of the current transfer (live HW counter).
 * On 8-bit width circular transfers, indicates the write position:
 * pos = size - remaining. NOTE: on H5/U5 (GPDMA) the counter is in bytes —
 * it coincides with elements only for CA_DMA_WIDTH_8. */
u32 ca_dma_get_remaining(ca_dma_ch_t ch);

/* Call from each DMA channel's ISR upon receiving the TC flag.
 * Invokes the registered callback (if present), then performs ca_sem_post()
 * on the channel semaphore. */
void ca_dma_isr_complete(u8 controller, u8 stream);

/* Transfer-complete callback — executed in ISR context: keep short, no blocking. */
typedef void (*ca_dma_cb_t)(void *arg);

/* Register a TC callback on the channel (NULL to remove it).
 * Invoked by ca_dma_isr_complete() BEFORE the sem_post: useful for
 * restoring the peripheral or posting to a queue from the ISR.
 * ca_dma_release() clears it. */
bool ca_dma_set_callback(ca_dma_ch_t ch, ca_dma_cb_t cb, void *arg);

/* Start circular DMA transfer (infinite): memory ↔ periph.
 * dir=CA_DMA_DIR_M2P: mem (MINC) → periph (fixed), repeats infinitely.
 * dir=CA_DMA_DIR_P2M: periph (fixed) → mem (MINC), repeats infinitely.
 * The DMA reloads automatically via hardware without CPU intervention.
 * DO NOT call ca_dma_wait() on circular channels.
 * Stop with ca_dma_stop_circular(). */
bool ca_dma_start_circular(ca_dma_ch_t ch,
                            void *mem, volatile void *periph,
                            u32 size, ca_dma_dir_t dir, ca_dma_width_t width);

/* Stop circular transfer and release the channel (= ca_dma_release). */
void ca_dma_stop_circular(ca_dma_ch_t ch);

#endif /* EN_DMA */
#endif /* DMA_H */
