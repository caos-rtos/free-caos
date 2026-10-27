// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    sai.h
 * @brief   Serial audio interface: SAI1 as a full-duplex I2S port
 ******************************************************************************
 * SAI1 in the layout of an audio codec: block A is the master transmitter
 * (SCK_A, FS_A, SD_A and MCLK_A out), block B the receiver, slave and
 * synchronous with A (SD_B in, clocks shared inside the SAI). Stereo, two
 * 32-bit slots per frame, 16/24/32 data bits, I2S or MSB-justified.
 *
 *   pins (all five Nucleo): PE2 MCLK_A, PE4 FS_A, PE5 SCK_A, PE6 SD_A, PE3 SD_B
 *   AF6, AF13 on U5. A loopback is one jumper SD_A -> SD_B (CN9-20 -> CN9-22).
 *
 * Samples travel as i32, one word per channel, left then right; the data is
 * right-aligned (`bits` low bits). ca_sai_xfer() and ca_sai_xfer_dma()
 * sign-extend what they receive; a stream's rx buffer holds the raw words
 * (16/24 bits zero-padded): (i32)(w << (32 - bits)) >> (32 - bits).
 *
 * The sample rate is approximated with the clocks already running and
 * ca_sai_init() returns the one obtained:
 *   F4/F7  PLLI2S (started here, N/Q/DIVQ chosen), MCLK = 256 fs
 *   H7     per_ck (HSI, 64 MHz)   \
 *   H5     PLL1 Q (50 MHz)         > MCLK = SCK, or 256/512 fs when that is closer
 *   U5     PLL1 P (160 MHz)       /
 * An exact audio PLL comes with a real codec.
 *
 * ca_sai_xfer() polls the FIFOs (8 words): any task that keeps the CPU for
 * longer than 8 words (83 us at 48 kHz stereo) overruns the receiver - the
 * console sending two characters is enough. Use DMA for audio.
 *
 * DMA buffers must be static, not on a task stack: on H7 the stacks live in
 * DTCM, which DMA1/DMA2 cannot reach.
 *
 * CA_SAI_SPDIF makes block A an S/PDIF (IEC 60958) transmitter on SD_A only,
 * block B unused: the data is forced to 24 bits, fs = SCK / 128 and the SAI
 * sends preambles and parity itself. Each i32 word is one subframe, left
 * then right, the 24-bit sample in [23:0] and the V, U, C bits in [26:24]
 * (CA_SAI_SPDIF_*); the first word of a stream is channel A of frame 0 (B
 * preamble), so a buffer of 384 words is one IEC 60958 block. Transmit
 * only, by stream: ca_sai_stream_start(tx, NULL, words). The kernel clock
 * is solved for 128 fs (F4/F7: no MCKDIV, PLLI2S VCO kept >= 272 MHz so its
 * P output can also clock an SPDIFRX up to 192 kHz).
 *
 * Present where the part has SAI1 (F446, F767, H7, H5, U5); elsewhere
 * ca_sai_init() returns 0. One user at a time, task context only.
 */
#ifndef SAI_H
#define SAI_H

#include "common.h"
#include "os_config.h"

#if EN_SAI

typedef enum {
    CA_SAI_I2S = 0,     /* Philips I2S: FS low = left, data one bit after FS */
    CA_SAI_MSB,         /* MSB-justified: FS high = left, data on the FS edge */
    CA_SAI_SPDIF        /* S/PDIF transmitter on SD_A, block B unused */
} ca_sai_proto_t;

typedef struct {
    u32            fs_hz;   /* requested sample rate */
    u8             bits;    /* 16, 24 or 32 data bits (ignored by CA_SAI_SPDIF: 24) */
    ca_sai_proto_t proto;
} ca_sai_config_t;

/* Error flags since ca_sai_init() (ca_sai_errors) */
#define CA_SAI_ERR_UDR   0x01U  /* transmitter underrun: FIFO empty at a slot */
#define CA_SAI_ERR_OVR   0x02U  /* receiver overrun: FIFO full at a slot */
#define CA_SAI_ERR_AFS   0x04U  /* receiver: frame sync earlier than expected */
#define CA_SAI_ERR_LFS   0x08U  /* receiver: frame sync later than expected */

/* CA_SAI_SPDIF: status bits of a subframe word, next to the 24-bit sample */
#define CA_SAI_SPDIF_V   (1UL << 24)    /* validity: 1 = sample not fit for conversion */
#define CA_SAI_SPDIF_U   (1UL << 25)    /* user data bit */
#define CA_SAI_SPDIF_C   (1UL << 26)    /* channel status bit */

u32  ca_sai_init(const ca_sai_config_t *cfg);   /* actual sample rate, 0 = not possible */
u32  ca_sai_fs(void);                           /* actual sample rate, 0 = not initialized */
u32  ca_sai_xfer(const i32 *tx, i32 *rx, u32 words, u32 timeout_ms);  /* polled, words received (not SPDIF) */
u32  ca_sai_errors(void);                       /* CA_SAI_ERR_* */
void ca_sai_stop(void);

#if EN_DMA
bool ca_sai_xfer_dma(const i32 *tx, i32 *rx, u32 words, u32 timeout_ms);  /* one block, both ways (not SPDIF) */
bool ca_sai_stream_start(const i32 *tx, i32 *rx, u32 words);  /* circular, both ways; SPDIF: rx NULL */
u32  ca_sai_stream_pos(void);                   /* next rx word the DMA writes (SPDIF: tx), 0..words-1 */
#endif

#endif /* EN_SAI */

#endif /* SAI_H */
