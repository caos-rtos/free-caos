// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file    qspi.h
 * @brief   QSPI/OCTOSPI driver — public API
 * @note    HW implementation: hw/<family>/drivers/qspi_<family>.c
 *
 *  Present on: F446/F7xx/H7xx (QUADSPI), H5xx/U5xx (OCTOSPI)
 *  Absent on:  F1xx, F407/F429 — functions return false
 *
 *  Pins (pinmap 0): the Zio CN10 QSPI positions, the same on every
 *  Nucleo-144 - 13 NCS, 15 CLK, 23 IO0, 21 IO1, 25 IO2, 19 IO3. The MCU
 *  pins behind them differ per board: see the table in each hw driver.
 *
 *  INTERRUPT-DRIVEN. No call busy-waits on the flash: the data phases are
 *  moved by the controller's FIFO interrupt, and the long waits - a sector
 *  erase is 45-400 ms, a page program up to 3 ms - run on the controller's
 *  automatic status polling, which raises an interrupt when WIP clears. The
 *  calling task sleeps on a semaphore meanwhile. Every call is therefore
 *  for TASK context (not pre-scheduler, not ISR), and the driver serialises
 *  concurrent callers with a mutex.
 *
 *  Typical sequence:
 *    ca_qspi_init(&cfg)
 *    ca_qspi_erase_sector(addr)
 *    ca_qspi_write(addr, buf, len)
 *    ca_qspi_read(addr, buf, len)
 *    u8 *p = (u8 *)ca_qspi_mmap_enable();   memory-mapped read: p[0..]
 *
 *  MEMORY-MAPPED: the flash appears at CA_QSPI_MMAP_BASE and reads like
 *  memory (also by DMA or as XIP code). Any erase/write/read call leaves
 *  memory-mapped mode first; call ca_qspi_mmap_enable() again after it.
 *
 *  DEVICES (cfg.device): the controller is a bus, not a flash. The hw layer
 *  runs any operation; what changes per device is the protocol on top:
 *    CA_QSPI_DEV_NOR      serial NOR flash: erase, page program (WREN + WIP
 *                         wait), read, read_id, memory-mapped READ.
 *    CA_QSPI_DEV_RAM      PSRAM / FRAM / MRAM: no erase, no WREN, no WIP;
 *                         read and write split at cfg.page_size; memory-
 *                         mapped read, and WRITE on OCTOSPI (H5/U5) only -
 *                         the QUADSPI (F446/F7/H7) maps read-only.
 *    CA_QSPI_DEV_RAW      anything else (displays, FPGA, NAND): only
 *                         ca_qspi_command(); the flash calls return false.
 *  ca_qspi_command() runs one raw operation on any device.
 *
 *  VERIFIED ON HARDWARE (09/10/2026) - everything else is TO BE TESTED:
 *    NOR  W25Q128 (EF 40 18) on NUCLEO H563/U575/F767/F446/H753: erase,
 *         page program, single 0x0B and dual 0x3B read, sample shift up to
 *         the top divider, memory-mapped read. Interrupt-driven engine on
 *         H753 (QUADSPI); H563/U575 (OCTOSPI), F767/F446 pending.
 *  TO BE TESTED (no part on the bench yet):
 *    NOR  quad modes (0x6B, 0xEB, 0x32), 4-byte addressing, other vendors.
 *    RAM  CA_QSPI_DEV_RAM on a PSRAM (e.g. APS6404): chunking at page_size,
 *         the tCEM limit (CS low <= 8 us on APS6404 - keep page_size small
 *         enough at the clock in use), memory-mapped write on OCTOSPI.
 *    RAW  ca_qspi_command() on a QSPI display (e.g. ST77903, RM67162):
 *         cmd_lines / addr_bytes per transfer, data-only streams.
 *
 *  Common flash commands (W25Qxx / IS25LPxx / compatible):
 *    cmd_read      = 0xEB  (Quad I/O Fast Read, 4-4-4)  or 0x6B (1-1-4)
 *    cmd_page_prog = 0x32  (Quad Page Program)           or 0x02 (single)
 *    cmd_sector_er = 0x20  (Sector Erase 4KB)
 *    cmd_wren      = 0x06
 *    cmd_rdsr      = 0x05
 ******************************************************************************
 */

#ifndef QSPI_H
#define QSPI_H

#include "def_mcu.h"
#include "os_config.h"

#if EN_QSPI

typedef struct {
    u8  clk_div;       /*!< Prescaler: periph_clk / (clk_div + 1)  */
    u8  flash_size;    /*!< log2(size_bytes) - 1, e.g. 23 for 16MB */
    u8  addr_bytes;    /*!< Address bytes: 3 (24-bit) or 4 (32-bit) */
    u8  dummy_cycles;  /*!< Dummy cycles for the read command      */
    u8  data_lines;    /*!< Read data lines: 1=single, 2=dual, 4=quad.
                            Page program uses 4 lines only when this is
                            4 (0x32), 1 otherwise: there is no dual page
                            program on W25Q/IS25 parts              */
    u8  cmd_read;      /*!< Fast read command, e.g. 0xEB or 0x6B */
    u8  cmd_page_prog; /*!< Page program, e.g. 0x32 or 0x02           */
    u8  cmd_sector_er; /*!< Sector erase, e.g. 0x20                  */
    u8  cmd_wren;      /*!< Write enable, 0x06                       */
    u8  cmd_rdsr;      /*!< Read status reg, 0x05                    */
    u8  pinmap;        /*!< 0 = Nucleo-144 Zio CN10, 1 = alternate; an
                            undefined value falls back to 0           */
    u8  addr_lines;    /*!< Read address lines: 0/1 = single (0x0B, 0x3B,
                            0x6B), 2 = dual (0xBB), 4 = quad (0xEB)   */
    u8  sample_shift;  /*!< CA_QSPI_SSHIFT_AUTO (0) / _ON / _OFF       */
    u8  device;        /*!< CA_QSPI_DEV_NOR (0) / _RAM / _RAW          */
    u16 page_size;     /*!< NOR: program page (0 = 256). RAM: largest
                            transfer under one CS (0 = 256)           */
} ca_qspi_cfg_t;

#define CA_QSPI_DEV_NOR         0U
#define CA_QSPI_DEV_RAM         1U
#define CA_QSPI_DEV_RAW         2U

/* Sample shift: read data sampled 1/2 SCK later, to absorb the round trip
 * (flash output delay + wiring + pads) that at high clock eats the half
 * period the controller samples in. SDR only - the driver never uses DTR.
 * AUTO turns it on above CA_QSPI_SSHIFT_AUTO_HZ, computed from the kernel
 * clock and clk_div; ON/OFF force it (e.g. to measure the margin). */
#define CA_QSPI_SSHIFT_AUTO     0U
#define CA_QSPI_SSHIFT_ON       1U
#define CA_QSPI_SSHIFT_OFF      2U
#define CA_QSPI_SSHIFT_AUTO_HZ  40000000U

/* Memory-mapped window: the same on every family that has the controller
 * (QUADSPI bank 1 on F446/F7/H7, OCTOSPI1 on H5/U5). */
#define CA_QSPI_MMAP_BASE       0x90000000UL

/* One controller operation, built by qspi.c and run by the hw layer:
 * instruction (always 1 line), optional address, dummy cycles, optional data.
 * Lines: 0 = phase absent, 1/2/4 = width. */
typedef struct {
    u8        cmd;
    u8        cmd_lines;     /* 0/1 = single, 2, 4 (QPI devices)  */
    u8        addr_bytes;    /* 0 = cfg.addr_bytes                */
    u8        addr_lines;
    u32       addr;
    u8        dummy;
    u8        data_lines;
    bool      read;          /* data phase direction             */
    u8       *rx;            /* read: destination                */
    const u8 *tx;            /* write: source                    */
    u32       len;           /* data bytes (0 with data_lines 0) */
} ca_qspi_xfer_t;

/* Called by the hw layer from its interrupt when the operation started by
 * ca_qspi_hw_start_xfer()/_autopoll() ends: ok false = transfer error. */
void ca_qspi_isr_done(bool ok);

/* Incremented by the hw layer on every QSPI interrupt entry (`qspi stat`). */
extern volatile u32 ca_qspi_isr_entries;

/* Signal order of ca_qspi_get_pin() - the order of the hw pin tables. */
typedef enum {
    CA_QSPI_PIN_CLK = 0, CA_QSPI_PIN_NCS, CA_QSPI_PIN_IO0,
    CA_QSPI_PIN_IO1, CA_QSPI_PIN_IO2, CA_QSPI_PIN_IO3, CA_QSPI_PIN_COUNT
} ca_qspi_pin_t;

/* Initializes the QSPI/OCTOSPI controller with the provided configuration.
 * Must be called before any other function.
 * Returns false if the driver is not supported on the current family. */
bool ca_qspi_init(const ca_qspi_cfg_t *cfg);

/* Erases the sector containing addr (sector size depends on the flash).
 * Sends WREN, then sector-erase, then waits for WIP=0. */
bool ca_qspi_erase_sector(u32 addr);

/* Writes len bytes from buf to address addr.
 * Automatically handles page boundaries (page program 256 bytes).
 * The area must have been previously erased. */
bool ca_qspi_write(u32 addr, const u8 *buf, u32 len);

/* Reads len bytes from addr into buf (indirect read). */
bool ca_qspi_read(u32 addr, u8 *buf, u32 len);

/* One raw operation on any device (display commands, PSRAM mode setup,
 * vendor registers...): instruction, optional address, dummy, optional data
 * in either direction. Interrupt driven like the rest; leaves memory-mapped
 * mode first. timeout_ms 0 = 100 ms. TO BE TESTED beyond the NOR.
 * A data buffer on a TASK STACK is limited to 256 bytes here (it goes
 * through the driver's bounce buffer, and a command without an address cannot
 * be split): use a static or heap buffer for longer streams. Read/write have
 * no such limit - they split addressed transfers by themselves. */
bool ca_qspi_command(const ca_qspi_xfer_t *x, u32 timeout_ms);

/* JEDEC ID (0x9F): manufacturer, type, capacity code. NOR only. */
bool ca_qspi_read_id(u8 id[3]);

/* Memory-mapped window with the cfg read command: returns CA_QSPI_MMAP_BASE,
 * or 0 if not initialised / not supported. Valid until the next erase,
 * write, read or ca_qspi_mmap_disable(). Read-only for NOR; for RAM it is
 * writable too on OCTOSPI (H5/U5, cmd_page_prog as the write command), still
 * read-only on QUADSPI (F446/F7/H7) - writes there go by ca_qspi_write(). */
u32  ca_qspi_mmap_enable(void);
void ca_qspi_mmap_disable(void);

/* The MCU pin the driver programs for signal `sig` under `pinmap` (same
 * fallback to 0 as the init). For tests that must probe the very pins the
 * driver uses, e.g. a `wire` command. False where there is no QSPI. */
bool ca_qspi_get_pin(u8 pinmap, ca_qspi_pin_t sig,
                     ca_gpio_port_t *port, ca_gpio_pin_t *pin);

#endif /* EN_QSPI */
#endif /* QSPI_H */
