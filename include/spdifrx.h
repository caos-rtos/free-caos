// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    spdifrx.h
 * @brief   S/PDIF receiver (SPDIFRX): IEC 60958 stream in, samples and status
 ******************************************************************************
 * One input, SPDIFRX_IN on PD7 (CN9-2 on the three Nucleo-144 that have it:
 * F446 and F767 IN0 AF8, H753 IN1 AF9). The receiver finds the symbol rate
 * by itself: there is no rate to configure, it is measured (ca_spdifrx_fs).
 *
 * Every read starts from scratch (STATE_IDLE -> RCV): the peripheral
 * synchronizes on the stream, then waits for a B preamble, so the first
 * word is always channel A of frame 0 of an IEC 60958 block. Words come
 * raw, one per subframe, left then right:
 *
 *   [23:0] sample  [24] PE  [25] V  [26] U  [27] C  [29:28] preamble
 *
 * CA_SPDIFRX_SAMPLE() gives the sign-extended 24-bit sample; a 16-bit
 * source is in [23:8].
 *
 * SPDIFRX_CLK must be >= 704 fs (11 clocks per symbol):
 *   F446   PLLI2S P (SPDIFRXSEL = 1), shared with the SAI  \  started here at
 *   F7     PLLI2S P (the only source), shared with the SAI /  172 MHz if off
 *   H7     HSI 64 MHz (SPDIFSEL = 11): up to 90 kHz streams
 * On F4/F7 a ca_sai_init() re-tunes PLLI2S: initialize the SAI first.
 *
 * The data register is two words deep: a polled read that loses the CPU
 * for one sample period (21 us at 48 kHz) overruns (CA_SPDIFRX_ERR_OVR).
 * Use DMA, into a static buffer (H7: DMA1/DMA2 cannot reach the DTCM).
 *
 * Present on F446, F746/F767 and H7; elsewhere ca_spdifrx_init() returns 0.
 * One user at a time, task context only.
 */
#ifndef SPDIFRX_H
#define SPDIFRX_H

#include "common.h"
#include "os_config.h"

#if EN_SPDIFRX

/* Received word (SPDIFRX_FMT0_DR, nothing masked) */
#define CA_SPDIFRX_PE           (1UL << 24)     /* parity error in this subframe */
#define CA_SPDIFRX_V            (1UL << 25)     /* validity bit */
#define CA_SPDIFRX_U            (1UL << 26)     /* user data bit */
#define CA_SPDIFRX_C            (1UL << 27)     /* channel status bit */
#define CA_SPDIFRX_PT(w)        (((u32)(w) >> 28) & 3U)    /* preamble: CA_SPDIFRX_PT_* */
#define CA_SPDIFRX_PT_B         1U              /* channel A, start of block */
#define CA_SPDIFRX_PT_M         2U              /* channel A */
#define CA_SPDIFRX_PT_W         3U              /* channel B */
#define CA_SPDIFRX_SAMPLE(w)    ((i32)((u32)(w) << 8) >> 8)

/* Channel status block: 192 bits of channel A, as 24 bytes, bit 0 first */
#define CA_SPDIFRX_CS_BYTES     24U

/* Error flags of the last sync, read or channel_status (ca_spdifrx_errors) */
#define CA_SPDIFRX_ERR_PAR      0x01U   /* parity error in a subframe */
#define CA_SPDIFRX_ERR_OVR      0x02U   /* data register overrun: words lost */
#define CA_SPDIFRX_ERR_FRM      0x04U   /* framing: bad biphase or preamble */
#define CA_SPDIFRX_ERR_SYNC     0x08U   /* synchronization failed (retries spent) */
#define CA_SPDIFRX_ERR_TMO      0x10U   /* no transition for too long: no signal */
/* Status bits of ca_spdifrx_hw_flags() besides the errors */
#define CA_SPDIFRX_HW_SYNCD     0x100U  /* synchronization done */
#define CA_SPDIFRX_HW_SBD       0x200U  /* B preamble seen */
#define CA_SPDIFRX_HW_STOP      (CA_SPDIFRX_ERR_FRM | CA_SPDIFRX_ERR_SYNC | CA_SPDIFRX_ERR_TMO)

u32  ca_spdifrx_init(void);                     /* SPDIFRX_CLK in Hz, 0 = not possible */
u32  ca_spdifrx_sync(u32 timeout_ms);           /* measured fs, 0 = no stream */
u32  ca_spdifrx_fs(void);                       /* fs measured by the last sync or read, 0 = none */
u32  ca_spdifrx_read(u32 *w, u32 words, u32 timeout_ms);       /* polled, words read */
u32  ca_spdifrx_channel_status(u8 *cs, u32 timeout_ms);        /* one whole block: CA_SPDIFRX_CS_BYTES, else 0 */
u32  ca_spdifrx_errors(void);                   /* CA_SPDIFRX_ERR_* */
void ca_spdifrx_stop(void);

#if EN_DMA
bool ca_spdifrx_read_dma(u32 *w, u32 words, u32 timeout_ms);   /* one block by DMA */
#endif

#endif /* EN_SPDIFRX */

#endif /* SPDIFRX_H */
