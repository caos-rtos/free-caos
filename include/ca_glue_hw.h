// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
#pragma once
/* ca_glue_hw.h — caOS HW/SW interface
 * Declares all functions implemented in hw/<family>/drivers/.
 * Do not call directly from application code. */
#include "caos.h"

/* ── ADC ──────────────────────────────────────────────────────────────────── */
#if EN_ADC
extern void ca_clock_hw_adc_init(u8 adc_num);                              /* enables the RCC clock for the indicated ADC peripheral */
extern bool ca_adc_hw_init_periph(u8 adc_num, u8 sample_time);             /* configures the ADC peripheral in polling mode */
extern void ca_gpio_hw_adc_pin_init(u8 adc_num, u8 channel);               /* configures the GPIO pin in analog mode for the ADC channel */
extern u16  ca_adc_hw_read(u8 adc_num, u8 channel);                        /* starts conversion and returns the 12-bit raw value */
extern bool ca_adc_hw_init_periph_isr(u8 adc_num, u8 sample_time, ca_adc_isr_cb_t cb); /* configures ADC in interrupt mode with callback */
extern void ca_adc_hw_start_isr(u8 adc_num, u8 channel);                   /* starts asynchronous conversion — the result arrives via callback */
extern void ca_adc_hw_irq_handler(u8 adc_num);                             /* ADC IRQ handler: reads the result and invokes the callback */
#endif

/* ── AES ──────────────────────────────────────────────────────────────────── */
#if EN_AES
extern bool ca_aes_hw_init(void);                                           /* initializes the hardware AES/CRYP peripheral */
extern bool ca_aes_hw_setup(ca_aes_ctx_t *ctx, bool encrypt);               /* loads key and IV into the peripheral for ECB/CBC */
extern bool ca_aes_hw_process(const u8 *in, u8 *out, u32 len);             /* encrypts/decrypts a block with the current configuration */
extern bool ca_aes_hw_gcm(ca_aes_ctx_t *ctx, bool encrypt,
                           const u8 *aad, u32 aad_len,
                           const u8 *in, u8 *out, u32 len,
                           u8 *tag);                                        /* encrypts/decrypts in GCM mode with AAD authentication and tag */
#endif

/* ── USB ──────────────────────────────────────────────────────────────────── */
#if EN_USB
extern void            ca_usb_fs_dev_clock_init(u32 usb_num);
extern void            ca_usb_fs_dev_gpio_init(u32 usb_num);
extern bool            ca_usb_fs_dev_init(u32 usb_num, const ca_usb_cb_t *cb);
extern bool            ca_usb_fs_dev_ep_open(u32 usb_num, u8 ep_addr, ca_usb_ep_type_t type, u16 mps);
extern bool            ca_usb_fs_dev_ep_write(u32 usb_num, u8 ep_addr, const u8 *buf, u16 len);
extern u16             ca_usb_fs_dev_ep_read(u32 usb_num, u8 ep_addr, u8 *buf, u16 max_len);
extern void            ca_usb_fs_dev_set_address(u32 usb_num, u8 addr);
extern ca_usb_speed_t  ca_usb_fs_dev_get_speed(u32 usb_num);
extern void            ca_usb_fs_dev_isr(u32 usb_num);
extern void            ca_usb_fs_dev_connect(u32 usb_num);
extern void            ca_usb_fs_dev_disconnect(u32 usb_num);
extern void            ca_usb_fs_dev_ep_stall(u32 usb_num, u8 ep_addr);
extern bool            ca_usb_fs_dev_ep_close(u32 usb_num, u8 ep_addr);
extern bool            ca_usb_fs_dev_ep_clear_stall(u32 usb_num, u8 ep_addr);
extern bool            ca_usb_fs_dev_ep_is_stalled(u32 usb_num, u8 ep_addr);
extern const ca_usb_dev_ops_t ca_usb_fs_dev_ops;
#endif /* EN_USB */

/* ── CAN ──────────────────────────────────────────────────────────────────── */

/* ── CLOCK ────────────────────────────────────────────────────────────────── */
extern int  ca_os_hw_clock_config(void);                                   /* configures PLL and system clock at boot */
extern void ca_clock_hw_deinit(void);                                     /* back to the reset clock tree (SYSCLK on HSI/MSIS, PLLs off): ca_os_end */
extern void ca_os_hw_periph_reset(void);                                  /* force + release the reset of every peripheral (RCC xxxRSTR): ca_os_end */
extern u32  ca_os_hw_clock_reduce_freq(u16 scale);                         /* reduces CPU frequency for power saving; returns the new freq Hz */
extern u32  ca_os_hw_clock_restore_freq(void);                             /* restores nominal CPU frequency; returns the freq Hz */
extern void ca_os_hw_clock_dump(void);                                     /* prints current clocks (AHB, APB1, APB2) to debug */

/* ── CRC32 ────────────────────────────────────────────────────────────────── */
#if EN_CRC
extern void ca_crc32_hw_init(void);                                        /* enables the clock and resets the hardware CRC peripheral */
#endif

/* ── DAC ──────────────────────────────────────────────────────────────────── */
#if EN_DAC
extern void ca_clock_hw_dac_init(u8 channel);                              /* enables the RCC clock for the DAC peripheral */
extern void ca_gpio_hw_dac_init(u8 channel);                               /* configures the GPIO pin in analog mode for the DAC channel */
extern bool ca_dac_hw_init_periph(u8 channel);                             /* initializes and enables the DAC channel */
extern void ca_dac_hw_write(u8 channel, u16 val);                          /* writes the raw value (0–4095) to the DAC channel */
extern bool ca_dac_hw_stop(u8 channel);                                    /* disables the DAC channel, releasing its output buffer */
extern bool ca_dac_hw_comp_init(u8 dac_channel, u8 hyst);                  /* COMP1: PB2 against the DAC channel; false = no comparator */
extern bool ca_dac_hw_comp_level(void);                                    /* COMP1 output */
extern void ca_dac_hw_comp_stop(void);                                     /* COMP1 off */
extern bool ca_adc_hw_stop(u8 adc_num);                                    /* stops conversions and disables the ADC (clears ADEN) */
extern bool ca_adc_hw_stop_isr(u8 adc_num);                                /* leaves the ADC on but disables its end-of-conversion interrupt */
#endif

/* ── DMA ──────────────────────────────────────────────────────────────────── */
#if EN_DMA
extern u8         ca_dma_hw_get_channel_count(void);                       /* returns the total number of DMA channels available on the MCU */
extern u8         ca_dma_hw_m2m_first_flat(void);                         /* first flat index able to do memory-to-memory (F4/F7: DMA2 only) */
extern u8         ca_dma_hw_ch_to_flat(ca_dma_ch_t ch);                   /* converts handle (controller+channel) to flat index */
extern ca_dma_ch_t ca_dma_hw_flat_to_ch(u8 flat);                         /* converts flat index to handle (controller+channel) */
extern void       ca_clock_hw_dma_init(u8 controller);                    /* enables the RCC clock for the indicated DMA controller */
extern bool       ca_dma_hw_copy_inc(u8 flat, const void *src, void *dst,
                                     u32 size, ca_dma_width_t width);      /* memory→memory DMA transfer with pointer increment */
extern bool       ca_dma_hw_copy_target(u8 flat, void *mem, volatile void *periph,
                                        u32 size, ca_dma_dir_t dir, ca_dma_width_t width); /* DMA transfer between memory and peripheral */
extern void       ca_dma_hw_release(u8 flat);                              /* frees the DMA channel and disables interrupts */
extern void       ca_dma_hw_abort(u8 flat);                                /* stops a running transfer, no TC, flags cleared, request kept */
extern bool       ca_dma_hw_set_request(u8 flat, u16 request);             /* associates the peripheral request line to the channel (DMA_REQ_NONE = SW/M2M) */
extern u32        ca_dma_hw_get_remaining(u8 flat);                        /* live HW counter: remaining elements (bytes on GPDMA) of the transfer */
extern void       ca_dma_hw_irq_enable(u8 flat);                           /* enables the completion interrupt for the DMA channel */
extern void       ca_dma_hw_irq_disable(u8 flat);                          /* disables the completion interrupt for the DMA channel */
extern bool       ca_dma_hw_start_circular(u8 flat, void *mem, volatile void *periph,
                                            u32 size, ca_dma_dir_t dir, ca_dma_width_t width); /* starts DMA transfer in circular (continuous) mode */
#endif

/* ── ETH ──────────────────────────────────────────────────────────────────── */
#if EN_ETH
extern bool ca_eth_hw_init      (const ca_eth_cfg_t *cfg);                 /* initializes Ethernet MAC, DMA and PHY */
extern bool ca_eth_hw_send      (u16 buf_idx);                             /* transmits the frame in the indicated DMA buffer */
extern bool ca_eth_hw_recv      (u16 *out_idx, u16 new_idx);               /* receives a frame and returns the index of the DMA buffer */
extern void ca_eth_hw_deinit    (void);                                    /* stops Ethernet DMA and MAC */
extern u16  ca_eth_hw_phy_read  (u8 phy, u8 reg);                         /* reads an MDIO register of the PHY */
extern void ca_eth_hw_phy_write (u8 phy, u8 reg, u16 val);                /* writes an MDIO register of the PHY */
extern bool ca_eth_hw_link_poll (void);                                    /* reads the link status from the PHY; true = link up */
extern void ca_eth_hw_dma_sync  (void);                                    /* stops the DMA, rebuilds the descriptor rings, restarts: back to the post-init state */
extern void ca_eth_hw_dbg_dump       (void);                               /* prints DMA/MAC registers to debug */
extern u16  ca_eth_hw_pool_held_count(void);                               /* returns the number of DMA buffers currently in use */
extern void ca_eth_hw_isr       (void);                                    /* Ethernet IRQ handler: notifies reception and clears flags */
#endif

/* ── EXTI ─────────────────────────────────────────────────────────────────── */
#if EN_EXTI
extern void ca_exti_hw_clock_init(void);                                   /* enables the SYSCFG/EXTI multiplexer clock */
extern void ca_exti_hw_select_source(u8 port, u8 pin);                     /* connects the GPIO pin to the corresponding EXTI line */
extern void ca_exti_hw_set_edge(u8 pin, ca_exti_edge_t edge);              /* configures the trigger edge (rising, falling, both) */
extern void ca_exti_hw_nvic_enable(u8 pin);                                /* enables the EXTI interrupt in the NVIC with default priority */
extern void ca_exti_hw_nvic_disable(u8 pin);                               /* disables the EXTI interrupt in the NVIC */
extern void ca_exti_hw_imr_enable(u8 pin);                                 /* enables the interrupt mask (IMR) for the EXTI line */
extern void ca_exti_hw_imr_disable(u8 pin);                                /* disables the interrupt mask (IMR) for the EXTI line */
#endif

/* ── FLASH ────────────────────────────────────────────────────────────────── */
#if EN_FLASH
extern bool ca_flash_hw_unlock(void);                                      /* removes the FLASH controller write protection */
extern void ca_flash_hw_lock(void);                                        /* restores the FLASH controller write protection */
extern bool ca_flash_hw_erase_sector(u32 addr);                            /* erases the sector containing addr (blocking) */
extern bool ca_flash_hw_write(u32 addr, const u8 *buf, u32 len);           /* writes len bytes to FLASH starting from addr (blocking) */
extern u16  ca_flash_hw_num_sectors(void);                                 /* returns the total number of MCU FLASH sectors */
extern u32  ca_flash_hw_sector_addr(u8 sector);                            /* returns the base address of the indicated sector */
extern u32  ca_flash_hw_sector_size(u8 sector);                            /* returns the size in bytes of the indicated sector */
extern void ca_flash_hw_nvic_enable(void);                                 /* enables FLASH_IRQn in the NVIC */
extern void ca_flash_hw_irq_enable(u32 addr);                              /* enables EOPIE and error interrupts for the bank containing addr */
extern void ca_flash_hw_irq_disable(void);                                 /* disables all FLASH interrupts */
extern bool ca_flash_hw_start_erase_irq(u32 addr);                        /* starts asynchronous erase; the IRQ signals completion */
extern u32  ca_flash_hw_start_write_irq(u32 addr, const u8 *buf, u32 remaining); /* starts asynchronous write; returns the consumed bytes */
extern u32  ca_flash_hw_irq_handler(void);                                 /* FLASH IRQ handler: reads SR and clears flags; returns error mask */
#endif

/* ── GPIO ─────────────────────────────────────────────────────────────────── */
extern GPIO_TypeDef* ca_gpio_get_io_port(ca_gpio_port_t port);                /* returns the pointer to the GPIO register of the indicated port */
extern bool ca_gpio_hw_init_port(ca_gpio_port_t port);                        /* enables the RCC clock of the GPIO port */
extern bool ca_gpio_hw_configure(ca_gpio_port_t port, ca_gpio_pin_t pin, ca_gpio_mode_t mode,
                                 ca_gpio_otype_t otype, ca_gpio_speed_t speed, ca_gpio_pull_t pull, u8 af); /* configures a GPIO pin with all parameters */

/* ── HASH ─────────────────────────────────────────────────────────────────── */
#if EN_HASH
extern bool ca_hash_hw_init(void);                                         /* enables the clock and resets the hardware HASH peripheral */
extern u32  ca_hash_hw_csr_count(void);                                    /* returns the number of CSR registers of the HASH peripheral */
extern bool ca_hash_hw_start(ca_hash_algo_t algo);                         /* selects the algorithm and starts a new hashing session */
extern void ca_hash_hw_feed(const u8 *data, u32 len);                      /* feeds whole words (len multiple of 4) to the HASH peripheral */
extern void ca_hash_hw_set_nblw(u8 valid_bytes);                           /* sets NBLW = valid bits (0..3 bytes) of the last word before DCAL */
extern bool ca_hash_hw_digest(u8 *digest, u32 *digest_len, ca_hash_algo_t algo); /* finalizes and reads the digest; false if timeout */
extern void ca_hash_hw_save_ctx(u32 *csr, u32 count, u32 *cr);             /* saves the HASH context to resume later */
extern void ca_hash_hw_restore_ctx(const u32 *csr, u32 count, u32 cr);    /* restores a previously saved HASH context */
#endif

/* ── I2C ──────────────────────────────────────────────────────────────────── */
#if EN_I2C
extern void ca_gpio_hw_i2c_init(u8 num, u8 alternate);                     /* configures SDA/SCL pins in alternate function open-drain mode; alternate selects the pin pair (0 = default) */
extern void ca_clock_hw_i2c_init(u8 num);                                  /* enables the RCC clock for the indicated I2C peripheral */
extern bool ca_i2c_hw_bus_recovery(u8 num, u8 alternate);                  /* clocks a slave stuck on SDA free on the pins of this num/alternate, then hands them back to the peripheral; true = bus idle */
extern bool ca_i2c_recover_pins(ca_gpio_port_t scl_p, ca_gpio_pin_t scl_n,
                                ca_gpio_port_t sda_p, ca_gpio_pin_t sda_n);      /* hw/common/i2c_recover.c: the 9-pulse + STOP sequence, pins left as open-drain GPIO */
extern bool ca_i2c_hw_init_periph(u8 num, ca_i2c_speed_t speed);           /* configures the I2C peripheral with the indicated speed */
extern bool ca_i2c_hw_transfer7(u8 num, u8 addr,
                                 const u8 *w, u16 wn,
                                 u8 *r, u16 rn);                           /* complete I2C transaction (write+read) with 7-bit address */
extern void ca_i2c_hw_enable_irq(u8 num);                                  /* enables I2C interrupts (event and error) in the NVIC */
extern bool ca_i2c_hw_slave_init(u8 num, u8 addr);                         /* configures the I2C peripheral in slave mode with own address */
extern void ca_i2c_hw_recv_handler(u8 num);                                /* slave event I2C IRQ handler: reads received bytes */
extern void ca_i2c_hw_err_handler(u8 num);                                 /* slave error I2C IRQ handler: resets the bus */
#endif

/* ── PWR ──────────────────────────────────────────────────────────────────── */
#if EN_PWR
extern void ca_pwr_hw_init(void);                                          /* enables the PWR block clock (where necessary) */
extern u32  ca_pwr_hw_max_freq(void);                                      /* returns the board's nominal SYSCLK in Hz */
extern u32  ca_pwr_hw_get_vos(void);                                       /* returns the current voltage scaling; 0xFF if not available */
extern bool ca_pwr_hw_standby_resumed(void);                               /* true if boot comes from Standby; clears the SBF flag */
extern const ca_pwr_periph_t *ca_pwr_hw_periph_table(u32 *count);          /* RCC peripheral table of the family */
extern bool ca_pwr_hw_enter_stop(void);                                    /* disables IRQ, enters STOP, returns to wake with IRQ still disabled */
extern void ca_pwr_hw_enter_standby(void);                                 /* enters STANDBY; does not return (wake = reset) */
extern void ca_pwr_hw_wake_pin_enable(void);                               /* enables the WKUP1 pin as a wake-up source */
extern void ca_pwr_hw_stop_wake_nvic(u32 wake_flags);                      /* re-enables in the NVIC only the wake IRQs (RTC/LPTIM) after the pre-Stop mask-all */
extern bool ca_pwr_hw_lptim_arm(u32 ms);                                   /* arms LPTIM1 one-shot on LSI as wake from Stop (milliseconds); false if not supported */
extern void ca_pwr_hw_lptim_disarm(void);                                  /* stops LPTIM1 and disables the IRQ after wake */
extern void ca_pwr_hw_dbg_dump(void);                                      /* prints low-power registers (PWR/RCC/RTC/core) for wake diagnosis */
#endif

/* ── QSPI ─────────────────────────────────────────────────────────────────── */
#if EN_QSPI
extern bool ca_qspi_hw_init(const ca_qspi_cfg_t *cfg);                    /* clock, pins, controller, NVIC */
extern bool ca_qspi_hw_start_xfer(const ca_qspi_xfer_t *x);                /* starts one operation; end -> ca_qspi_isr_done() */
extern bool ca_qspi_hw_start_autopoll(u8 cmd, u8 mask, u8 match);          /* polls a status byte until (sr & mask) == match, then IRQ */
extern void ca_qspi_hw_abort(void);                                        /* stops whatever runs, interrupts off */
extern u32  ca_qspi_hw_mmap(bool on, const ca_qspi_xfer_t *rd, const ca_qspi_xfer_t *wr); /* memory-mapped (wr NULL = read-only); returns the base or 0 */
extern bool ca_qspi_hw_get_pin(u8 pinmap, u8 sig, ca_gpio_port_t *port, ca_gpio_pin_t *pin); /* MCU pin of signal sig (ca_qspi_pin_t order) */
#endif

/* ── RNG ──────────────────────────────────────────────────────────────────── */
#if EN_RNG
extern bool ca_rng_hw_init(void);                                          /* enables the clock and initializes the hardware RNG peripheral */
extern bool ca_rng_hw_get(u32 *val);                                       /* reads a 32-bit random value; false if error or timeout */
#endif

/* ── RTC ──────────────────────────────────────────────────────────────────── */
#if EN_RTC
extern bool ca_rtc_hw_init(ca_rtc_clk_src_t src);                         /* initializes the RTC with the indicated clock source (LSI/LSE) */
extern bool ca_rtc_hw_set(const ca_rtc_dt_t *dt);                          /* sets date and time in the RTC peripheral */
extern bool ca_rtc_hw_get(ca_rtc_dt_t *dt);                                /* reads date and time from the RTC peripheral */
extern bool ca_rtc_hw_get_sub(ca_rtc_dt_t *dt, u32 *sub, u32 *sub_div);  /* date/time + fraction of second sub/sub_div */
extern bool ca_rtc_hw_calibrate(i32 ppm);                                  /* applies frequency correction to the RTC oscillator in ppm */
extern bool ca_clock_hw_lse_lock(void);                                    /* lock the system clock RC to the running LSE (U5 MSI PLL-mode); false = not on this family */
extern bool ca_rtc_hw_set_prescaler(u32 div_a, u32 div_s);                 /* one second = div_a*div_s RTCCLK cycles (async 1..128, sync 1..32768) */
extern bool ca_rtc_hw_lse_on(bool on);                                   /* LSE oscillator on/off, RTC source untouched; returns LSERDY */
extern ca_rtc_clk_src_t ca_rtc_hw_get_src(void);                          /* source kept by the backup domain; CA_RTC_CLK_AUTO = none (cold) */
extern bool ca_rtc_hw_set_alarm(u8 alarm_id, const ca_rtc_alarm_t *alarm, ca_rtc_alarm_cb_t cb); /* configures an RTC alarm with callback */
extern bool ca_rtc_hw_clear_alarm(u8 alarm_id);                            /* disables and clears the indicated RTC alarm */
#endif

/* ── SDIO ─────────────────────────────────────────────────────────────────── */
#if EN_SDIO
extern bool ca_sdio_hw_init(void);                                         /* initializes the SDIO/SDMMC peripheral and detects the SD card */
extern bool ca_sdio_hw_read_block(u32 block, u8 *buf);                     /* reads a 512-byte block from the SD */
extern bool ca_sdio_hw_write_block(u32 block, const u8 *buf);              /* writes a 512-byte block to the SD */
extern bool ca_sdio_hw_write_blocks(u32 block, const u8 *buf, u32 count);  /* writes count blocks with CMD25 multi-block */
extern u32  ca_sdio_hw_get_block_count(void);                              /* returns the total number of blocks of the SD card */
#endif

/* ── SPI ──────────────────────────────────────────────────────────────────── */
#if ( EN_SPI || EN_SPI_DMA )
extern void ca_clock_hw_spi_init(u8 num);                                  /* enables the RCC clock for the indicated SPI peripheral */
extern void ca_gpio_hw_spi_init(u8 num, bool alternate);                   /* configures MOSI/MISO/SCK pins in AF; alternate=true → alternate mapping (see spi.h) */
extern bool ca_spi_hw_init_periph(u8 num, ca_spi_br_t br, u8 cpol, u8 cpha); /* initializes the SPI peripheral in master mode */
extern bool ca_spi_hw_init_periph_slave(u8 num, u8 cpol, u8 cpha,
                                        u16 frame_size, ca_spi_slave_cb_t cb); /* initializes the SPI peripheral in slave mode with callback */
extern bool ca_spi_hw_stop(u8 num);                                        /* disables the SPI (SPE=0) and its interrupt, draining any transfer in flight */
extern bool ca_spi_hw_xfer(u8 num, u8 data, u8 *rx);                      /* full-duplex byte; false if not done within CA_SPI_XFER_TIMEOUT_MS */
extern void ca_spi_hw_slave_tx_fill(u8 num, u8 fill);                    /* slave: byte answered on MISO for every byte received, 0 = none */
extern void ca_spi_hw_slave_sync(u8 num);
extern u8   ca_spi_hw_slave_sync_pending(u8 num);                         /* 1 if a byte was already waiting at the last sync */                                 /* slave: frame start (CS) - drop partial frame, clear overrun */
extern void ca_spi_hw_recv_handler(u8 num);                                /* SPI slave IRQ handler: reads the received byte and calls the callback */
#endif
#if EN_SPI_DMA
extern bool ca_spi_hw_dma_bind(u8 spi_num, i16 *rx_flat, u16 *rx_req,
                               i16 *tx_flat, u16 *tx_req);                 /* DMA channel (flat, -1 = any) + SPI RX/TX request */
extern volatile void *ca_spi_hw_dma_rx_addr(u8 spi_num);                   /* RX data register address for DMA (DR/RXDR) */
extern volatile void *ca_spi_hw_dma_tx_addr(u8 spi_num);                   /* TX data register address for DMA (DR/TXDR) */
extern void ca_spi_hw_dma_prepare(u8 spi_num, u16 len, bool rx_en);        /* pre-arm: RXDMAEN (+TSIZE=len and SPE=0 on IP v3) */
extern void ca_spi_hw_dma_kick(u8 spi_num);                                /* post-arm: TXDMAEN (+SPE/CSTART on IP v3) — the transfer starts here */
extern void ca_spi_hw_dma_finish(u8 spi_num, bool rx_en);                  /* waits for shift end (EOT/BSY), clear flag, restores idle state */
#endif

/* ── TEMP SENSOR ──────────────────────────────────────────────────────────── */
#if ENABLE_SENSOR
extern bool ca_temp_hw_init(void);                                         /* enables the internal temperature sensor and the Vrefint channel */
extern u16  ca_temp_hw_read_raw(void);                                     /* reads the raw ADC value of the internal temperature sensor */
extern u16  ca_temp_hw_read_vrefint(void);                                 /* reads the raw ADC value of the internal reference Vrefint */
extern i32  ca_temp_hw_convert(u16 ts_raw, u16 vrefint_raw);               /* converts raw values to degrees Celsius (compensated) */
extern bool ca_temp_hw_dts_init(void);                                     /* DTS on, false when the part has none */
extern u32  ca_temp_hw_dts_sample(void);                                   /* one fresh DTS measurement (raw count), 0 on timeout */
extern i32  ca_temp_hw_dts_centi(u32 raw);                                 /* raw count -> centi-degC at the current clock */
extern u32  ca_temp_hw_dts_raw(i32 centi);                                 /* centi-degC -> raw count at the current clock */
extern void ca_temp_hw_dts_alarm(u32 raw_hot, u32 raw_cold, bool hot_on, bool cold_on,
                                 ca_temp_alarm_cb_t cb);                  /* thresholds + IRQ, cb NULL = off */
#endif

/* ── TIMER ────────────────────────────────────────────────────────────────── */
#if EN_TIMER
extern bool ca_clock_hw_timer_init      (u8 num);                          /* enables the RCC clock for the indicated timer */
extern bool ca_timer_hw_init_periodic   (u8 num, u32 period_us);           /* configures the timer in periodic mode with period in microseconds */
extern bool ca_timer_hw_init_oneshot    (u8 num, u32 delay_us);            /* configures the timer in one-shot mode with delay in microseconds */
extern bool ca_timer_hw_init_counter    (u8 num);                          /* configures the timer as an external event counter */
extern bool ca_timer_hw_init_encoder    (u8 num);                          /* configures the timer in quadrature encoder mode */
extern bool ca_timer_hw_init_counter_irq(u8 num, u32 preset);              /* configures the timer as a counter with interrupt upon reaching the preset */
extern bool ca_timer_hw_init_pwm        (u8 num, u8 ch, u32 freq_hz);      /* configures channel ch (1-4) in PWM mode 1 with preload; false if the channel does not exist */
extern bool ca_timer_hw_set_pwm         (u8 num, u8 ch, u16 duty_pm);      /* sets the channel duty cycle in permille 0-1000 (CCR preload, glitch-free) */
extern bool ca_timer_hw_start           (u8 num);                          /* starts the timer; false if the timer has not been initialized */
extern bool ca_timer_hw_stop            (u8 num);                          /* stops the timer without resetting the counter */
extern void ca_timer_hw_deinit          (u8 num);                          /* stops and disables the timer, releasing resources */
extern u32  ca_timer_hw_get_cnt         (u8 num);                          /* reads the current timer counter value */
extern bool ca_timer_hw_init_capture    (u8 num, u8 ch, u32 src, u32 psc); /* input capture on ch: src 0 pin / 1 LSE / 2 LSI, psc = ICxPSC; false if unsupported */
extern u32  ca_timer_hw_clock_hz        (u8 num);                          /* counting rate of timer num (PSC = 0) */
#endif

/* ── LPTIM ────────────────────────────────────────────────────────────────── */
#if EN_LPTIM
extern bool ca_clock_hw_lptim_init(u8 num);                                /* turns on LSI, RCC clock, and LSI kernel clock for the indicated LPTIM; false if not present */
extern bool ca_lptim_hw_init      (u8 num, u32 ms, bool periodic, bool wake); /* validates ms (1..262144) and saves prescaler/ARR/mode/wake; does not start */
extern bool ca_lptim_hw_start     (u8 num);                                /* complete sequence of enable + CNTSTRT (periodic) or SNGSTRT (oneshot) */
extern bool ca_lptim_hw_stop      (u8 num);                                /* disables the counter (CNT restarts from 0 at the next start) */
extern void ca_lptim_hw_deinit    (u8 num);                                /* stops, disables NVIC, and turns off the peripheral clock */
extern u32  ca_lptim_hw_get_cnt   (u8 num);                                /* reads CNT with double read (asynchronous clock) */
#endif

/* ── LPUART ───────────────────────────────────────────────────────────────── */
#if EN_LPUART
extern bool ca_lpuart_hw_gpio_init(bool alternate_gpio);                   /* configures TX/RX pins in alternate function; alternate_gpio picks the 2nd pin map (H7: PB6/PB7); false if LPUART is not present */
extern bool ca_lpuart_hw_init     (u32 baud, bool wake);                   /* HSI16+HSIKERON, kernel clock, BRR, RXNEIE (+UESM/EXTI if wake); starts */
extern void ca_lpuart_hw_deinit   (void);                                  /* stops, disables NVIC, and turns off peripheral clock and HSIKERON */
extern u16  ca_lpuart_hw_put      (p_str message);                         /* transmits in polling (TXE/TC) */
extern i32  ca_lpuart_hw_recv     (u32 *rx_data);                          /* 0 if byte is valid in *rx_data; handles/clears ORE */
#endif

/* ── UART ─────────────────────────────────────────────────────────────────── */
#if ( ENABLE_DBG_UART || ENABLE_UART || EN_UART_DMA )
extern bool ca_uart_hw_configure_gpio(u32 uart_num, u8 alternate);         /* configures TX/RX pins in alternate function mode; alternate selects the pin pair (0 = default) */
extern bool ca_uart_hw_init(u32 uart_num, u32 baudrate, USART_TypeDef **_instance); /* initializes the UART peripheral at the indicated baudrate */
extern u16  ca_uart_hw_put(USART_TypeDef **_instance, p_str message);      /* transmits a string via UART in polling */
extern u32  ca_uart_hw_recv_handler(USART_TypeDef **_instance, u32 *rx_data); /* one received byte: 0 = read, 1 = none waiting, 2 = one dropped for a line error, more may wait (FIFO parts) */
/* RX line-error counters — defined in caos/drivers/src/uart.c, incremented by
 * ca_uart_hw_recv_handler in every family. Shown by the CLI "cli_stat". */
extern volatile u32 ca_uart_rx_err_ore;   /* overrun: byte lost, RX wedged until cleared */
extern volatile u32 ca_uart_rx_err_fe;    /* framing: bad stop bit -> baud rate or noise */
extern volatile u32 ca_uart_rx_err_ne;    /* noise detected while sampling */
extern volatile u32 ca_uart_rx_err_pe;    /* parity (only when parity is enabled) */
#endif
#if EN_UART_DMA
extern bool ca_uart_hw_dma_bind(u32 uart_num, i16 *rx_flat, u16 *rx_req,
                                i16 *tx_flat, u16 *tx_req);                /* DMA channel (flat, -1 = any) + RX/TX request of the uart */
extern volatile void *ca_uart_hw_dma_rx_addr(USART_TypeDef *inst);         /* RX data register address for DMA (DR/RDR) */
extern volatile void *ca_uart_hw_dma_tx_addr(USART_TypeDef *inst);         /* TX data register address for DMA (DR/TDR) */
extern void ca_uart_hw_dma_init(USART_TypeDef *inst);                      /* enables CR3.DMAT; RX remains in header mode (RXNEIE) */
extern void ca_uart_hw_dma_rx_on(USART_TypeDef *inst);                     /* payload phase: RXNEIE off, CR3.DMAR on */
extern void ca_uart_hw_dma_rx_off(USART_TypeDef *inst);                    /* frame end: CR3.DMAR off, RXNEIE on */
#endif

/* ── WDT ──────────────────────────────────────────────────────────────────── */
#if EN_WDT
extern bool ca_wdt_hw_init(u32 timeout_ms, ca_wdt_mode_t mode, ca_wdt_type_t type); /* initializes the watchdog with the indicated timeout */
extern bool ca_wdt_hw_enable(void);                                        /* starts the watchdog; after this call it must be reloaded periodically */
extern void ca_wdt_hw_reload(void);                                        /* reloads the watchdog (kick) to prevent reset */
extern u32  ca_wdt_hw_get_timeout_ms(void);                                /* returns the effective timeout in milliseconds after HW rounding */
extern bool ca_wdt_hw_is_enabled(void);                                    /* returns true if the watchdog is active and can no longer be disabled */
extern u32  ca_wdt_hw_max_timeout_ms(ca_wdt_type_t type);                  /* longest timeout the family supports for that type, in ms */
extern ca_wdt_reset_t ca_wdt_hw_reset_cause(bool clear);                   /* watchdog that caused the last reset; clear = clear ALL the RCC reset flags */
extern void ca_wdt_hw_on_early(ca_wdt_early_cb_t cb, void *arg);           /* callback of the early warning interrupt (ISR context) */
extern bool ca_wdt_hw_early_supported(ca_wdt_type_t type);                 /* whether that watchdog has an early warning interrupt here */
#endif /* EN_WDT */

/* Backup registers (bkp.h): always built, no EN_ flag */
extern u32  ca_bkp_hw_count(void);                                         /* backup registers on this part */
extern u32  ca_bkp_hw_read(u32 idx);                                       /* backup register idx (in range, checked by the caller) */
extern void ca_bkp_hw_write(u32 idx, u32 value);                           /* unlocks the backup domain (DBP) and writes register idx */
extern u32  ca_bkp_hw_sram_size(void);                                    /* backup SRAM bytes, 0 = none */
extern u8  *ca_bkp_hw_sram_open(void);                                    /* clock + backup domain write access on, returns the base */
extern bool ca_bkp_hw_sram_retain(void);                                  /* backup regulator on (Standby/VBAT retention), false if not possible */

/* CORDIC coprocessor (cordic.h): always built, no EN_ flag; H5/U5 only, stubs elsewhere */
extern bool ca_cordic_hw_init(void);                                       /* clock on; false when the part has no CORDIC */
extern void ca_cordic_hw_calc(u32 func, u32 scale, u32 nargs, u32 nres,
                              i32 arg1, i32 arg2, i32 *res1, i32 *res2);    /* one q1.31 operation, caller holds the lock */

/* Serial audio interface (sai.h): SAI1, block A master TX, block B sync slave RX;
 * F446/F7/H7/H5/U5, stubs elsewhere */
#if EN_SAI
extern u32  ca_sai_hw_init(u32 fs_hz, u32 bits, ca_sai_proto_t proto);     /* clock, pins, both blocks set up and off (SPDIF: A only); actual fs, 0 = no SAI */
extern void ca_sai_hw_start(bool dma);                                     /* RX FIFO flushed, B (not in SPDIF) then A on (DMAEN when dma) */
extern void ca_sai_hw_stop(void);                                          /* A and B off at the end of the frame, FIFOs flushed */
extern bool ca_sai_hw_put(u32 w);                                          /* one word into A's FIFO, false when full */
extern bool ca_sai_hw_get(u32 *w);                                         /* one word from B's FIFO, false when empty */
extern u32  ca_sai_hw_errors(void);                                        /* CA_SAI_ERR_* since the last call, cleared */
#if EN_DMA
extern bool ca_sai_hw_dma_bind(i16 *tx_flat, u16 *tx_req, i16 *rx_flat, u16 *rx_req); /* DMA channel (flat, -1 = any) + request, A (TX) and B (RX) */
extern volatile void *ca_sai_hw_dr(bool rx);                               /* data register of B (rx) or A */
extern u32  ca_sai_hw_dma_count_div(void);                                 /* ca_dma_get_remaining() units per word: 4 on GPDMA, 1 elsewhere */
#endif
#endif

/* S/PDIF receiver (spdifrx.h): SPDIFRX input 0 (PD7) on F446/F7/H7, stubs elsewhere */
#if EN_SPDIFRX
extern u32  ca_spdifrx_hw_init(void);                                      /* clock, pin, CR set up in STATE_IDLE; SPDIFRX_CLK Hz, 0 = no SPDIFRX */
extern void ca_spdifrx_hw_state(u32 en, bool dma);                         /* SPDIFRXEN = en (0 idle, 1 sync, 3 receive), RXDMAEN = dma */
extern u32  ca_spdifrx_hw_flags(void);                                     /* CA_SPDIFRX_ERR_* | CA_SPDIFRX_HW_*, the clearable ones cleared */
extern bool ca_spdifrx_hw_get(u32 *w);                                     /* one data word (FMT0), false when RXNE = 0 */
extern bool ca_spdifrx_hw_get_cs(u32 *csr);                                /* raw SPDIFRX_CSR ([24] SOB, [23:16] CS, [15:0] user), false when CSRNE = 0 */
extern u32  ca_spdifrx_hw_width5(void);                                    /* SR WIDTH5: SPDIFRX_CLK periods in 5 symbols */
#if EN_DMA
extern bool ca_spdifrx_hw_dma_bind(i16 *flat, u16 *req);                   /* data DMA channel (flat, -1 = any) + request */
extern volatile void *ca_spdifrx_hw_dr(void);                              /* SPDIFRX_FMT0_DR */
#endif
#endif

/* Chrom-ART accelerator (dma2d.h): F767/F746, H7, U5, stubs elsewhere */
#if EN_DMA2D
extern bool ca_dma2d_hw_init(void);                                         /* clock, NVIC; false = no DMA2D */
extern bool ca_dma2d_hw_src(bool bg, const ca_dma2d_src_t *s, u32 w);       /* FG/BG address, offset, PFC, color; false = flag not on this part */
extern u32  ca_dma2d_hw_clut(bool bg, const ca_dma2d_src_t *s);             /* CLUT by CPU or DMA2D, 0 = loaded, else CA_DMA2D_ERR_* */
extern bool ca_dma2d_hw_dst(const ca_dma2d_dst_t *d, u32 w, u32 color);     /* output address, offset, PFC, fill color; false = flag not on this part */
extern void ca_dma2d_hw_start(u32 mode, u32 w, u32 h);                      /* NLR, MODE, interrupts, START */
extern u32  ca_dma2d_hw_irq(void);                                          /* ISR read and cleared: CA_DMA2D_ERR_* | CA_DMA2D_HW_TC */
extern void ca_dma2d_hw_abort(void);                                        /* ABORT, until START falls */
extern bool ca_dma2d_hw_busy(void);                                         /* START still set */
#endif

/* JPEG codec (jpeg.h): F767, H7, stubs elsewhere */
#if EN_JPEG
extern bool ca_jpeg_hw_init(void);                                          /* clock, core enabled; false = no codec */
extern void ca_jpeg_hw_reset(void);                                         /* stop, FIFOs flushed, flags cleared, core re-enabled */
extern void ca_jpeg_hw_tables(const u32 *q0, const u32 *q1, const u32 *dht,
                              const u32 *ac0, const u32 *ac1, const u32 *dc0, const u32 *dc1); /* encoder memories, words as built by jpeg.c */
extern void ca_jpeg_hw_encode(u16 w, u16 h, ca_jpeg_css_t css, u32 mcus);   /* CONFR1..7 for encoding with header, START */
extern void ca_jpeg_hw_decode(void);                                        /* decoding with header parsing, START */
extern u32  ca_jpeg_hw_status(void);                                        /* CA_JPEG_HW_* */
extern void ca_jpeg_hw_put(u32 w);                                          /* input FIFO */
extern u32  ca_jpeg_hw_get(void);                                           /* output FIFO */
extern void ca_jpeg_hw_header(u16 *w, u16 *h, u32 *nf, u32 *hsf, u32 *vsf); /* parsed: size, components, Y sampling */
#endif

/* Filter math accelerator (fmac.h): EN_FMAC, paid only; H5/U5 only, stubs elsewhere */

/* Operational amplifiers (opamp.h): always built, no EN_ flag; U5/H7 only, stubs elsewhere */
extern u32  ca_opamp_hw_count(void);                                       /* instances on this part, 0 = none */
extern bool ca_opamp_hw_init(u32 num, bool from_dac, u32 gain_code);       /* gain_code 0 follower, 1..4 = x2..x16 */
extern void ca_opamp_hw_stop(u32 num);                                     /* disables OPAMPnum */
extern u8   ca_opamp_hw_adc_channel(u32 num, u8 *adc_num);                 /* ADC channel seeing the output, 0 = none */


/* ── MEMORY MAP ───────────────────────────────────────────────────────────── */
/* Not gated: the physical memory map is a fact of the part, always available.
 * Each family fills it in from its own linker script (cross-checked against
 * the datasheet) - ORIGIN/LENGTH are linker directives and do not survive to
 * runtime, so the constants live here with the other per-MCU facts. */
extern u32 ca_hw_memmap_get(ca_mem_range *out, u32 max);                   /* physical memory areas of this MCU; returns how many were written */

/* ── ONBOARD LEDs ─────────────────────────────────────────────────────────── */
/* One implementation for every family (hw/common/led.c): a LED is a GPIO pin,
 * so only WHICH pin and WHICH polarity change, and those live in objmap.h.
 * LED numbers are 1-based; ca_led_* validates them before calling in here. */
#if EN_LED
extern u8   ca_hw_led_count(void);                                         /* LEDs this board actually carries, 0 if none */
extern void ca_hw_led_init(void);                                          /* configure every declared LED as output, all off */
extern void ca_hw_led_on(u8 led_num);                                      /* caller guarantees 1 <= led_num <= count */
extern void ca_hw_led_off(u8 led_num);                                     /* caller guarantees 1 <= led_num <= count */
extern void ca_hw_led_toggle(u8 led_num);                                  /* caller guarantees 1 <= led_num <= count */
extern bool ca_hw_led_get(u8 led_num);                                     /* logical state (on/off), not the pin level */
#endif
