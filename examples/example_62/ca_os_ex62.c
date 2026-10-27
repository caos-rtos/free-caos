// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex62.c
 * @brief          : Example 62 — LPUART low-power console with wake from Stop
 ******************************************************************************
 * @details
 * The system CLI console is on LPUART1 (115200 8N1, kernel HSI16),
 * configured in os_config.h:
 *
 *   STDOUT_ON_LPUART 1   → STDOUT = ca_lpuart_put
 *   STDIN_ON_LPUART  1   → STDIN  = ca_cli_data_from_isr
 *   EVAL_LPUART_STD      → ca_lpuart_std_cli() called by ca_os_init:
 *                          init LPUART_STD_BAUD, on_rx → ca_cli_rx_byte,
 *                          wake = LPUART_STD_WAKE
 *
 * The LPUART is wake-armed: from the terminal enter
 *
 *   pwr stop 60
 *
 * and the MCU enters STOP mode (RTC wake at 60 s safety timeout) — by pressing
 * ANY key the MCU wakes up immediately, the byte is delivered to the CLI
 * and the console responds. The baudrate is on HSI16+HSIKERON:
 * no retune after pwr clock changes (works with "pwr freq").
 *
 * A task prints the uptime every 15 s to show that the console is alive.
 *
 * LPUART1 pins per board (TX/RX, 3.3V):
 *   NUCLEO-U575ZI-Q  PA2/PA3  AF8
 *   NUCLEO-H563ZI    PB6/PB7  AF8
 *   NUCLEO-H753ZI    PB6/PB7  AF8 = CN10-14/16 (D1/D0), via LPUART_STD_ALT_GPIO.
 *                    NOT PA9/PA10 (the RM default): PA9 is OTG_FS VBUS here.
 *
 * ⚠️ The wiring is NOT the same on every board, and it used to be written here
 * as if it were: one line read "U575: PA9→PA2, PA10→PA3   H563/H753: PD8→TX,
 * PD9→RX", lumping two boards together. On the H563 that recipe cannot work at
 * all - see below. A line that covers two boards has been checked on one.
 *
 * There is also no "wire" command in this example, and there cannot be a
 * useful one: `wire` runs on the CLI, and here the CLI is exactly what the
 * wiring is meant to bring up. Nothing to type it on until it already works.
 *
 * ── U575: 2 jumpers from the ST-LINK VCP ──────────────────────────────────
 * Turn off USB (EN_USB=0) and the dbg UART (the VCP pins stay high impedance
 * on the MCU side), then jump TX-USART→LPUART TX and RX-USART→LPUART RX (the
 * TX/RX crossover is already done between ST-LINK and MCU): PA9→PA2, PA10→PA3.
 *
 * U575 connector pins (UM2861 Rev 10, Tables 17 to 20):
 *
 *      PA9  = CN12-21  (VCP TX)  ---->  PA2 = CN9-3   (Zio A1, LPUART1 TX)
 *      PA10 = CN12-33  (VCP RX)  ---->  PA3 = CN9-1   (Zio A0, LPUART1 RX)
 *
 * The LPUART side is on the Zio: PA2/PA3 come out as the ADC inputs A1/A0
 * (Table 17), and SB57 is ON by default (Table 20) so A1 really is PA2.
 * The VCP side is NOT: PA9/PA10 have no Zio pin at all. Table 18 note 1 says
 * the D0/D1 signals default to LPUART1 on PG7/PG8, while USART1 on PA9/PA10
 * goes straight to the STLINK-V3E - the ST morpho CN12 is the only place
 * those two are reachable. (Both ends can also be taken on CN12 alone:
 * PA2 = CN12-35, PA3 = CN12-37.)
 *
 * ⚠️ The board silkscreen calls PG7/PG8 "USART_A" and brings them out as
 * D1/D0 on Zio CN10-14 / CN10-16, but ca_lpuart_hw_gpio_init() in
 * hw/u5xx/drivers/lpuart_u5xx.c programs PA2/PA3 and ignores the alternate
 * flag - U5 has a single mapping there. Wire PA2/PA3, not PG7/PG8.
 *
 * ── H563: a USB-serial adapter on the Zio, or eight solder bridges ────────
 *
 * ⚠️ DO NOT try the jumper recipe here. Tried on hardware 2026-09-01: jumpers
 * fitted, nothing came out, and the board was not at fault. UM3115 Table 12,
 * column "ST morpho connection", says PD8 (CN12-10) and PD9 (CN11-69) are
 * connected only with SB13/SB75 ON, and those ship OFF: the morpho holes are
 * there and lead nowhere, so the jumpers were bridging two disconnected pins.
 * Worse, the recipe is impossible rather than merely wrong - SB13 ON requires
 * SB23+SB24 OFF, while it is SB24 ON that carries PD8 to the VCP. The two are
 * mutually exclusive by design: no solder-bridge configuration puts PD8 on the
 * morpho and on the VCP at once.
 *
 * The board has its own way instead. UM3115 par.11.5 "Virtual COM port (VCP):
 * LPUART1/USART3" - the VCP is SELECTABLE between the two peripherals. As
 * shipped, USART3 (PD8/PD9) is on the VCP because it supports Bootloader mode,
 * and LPUART1 (PB6/PB7) goes to the ARDUINO D0/D1 pins so as not to interfere.
 *
 * NO SOLDERING - a USB-serial adapter (3.3 V) on the Zio, which is where
 * LPUART1 already comes out as shipped:
 *
 *      PB6 = CN10-14  (silkscreen D1, LPUART1 TX)  ---->  adapter RX
 *      PB7 = CN10-16  (silkscreen D0, LPUART1 RX)  <----  adapter TX
 *      GND = CN10-9 (or any board GND)             ----   adapter GND
 *
 * TX and RX cross here: there is no ST-LINK in between to do it for us, unlike
 * the U575 jumper case above. Terminal 115200 8N1, LPUART_STD_BAUD.
 *
 * SOLDERING - move eight bridges to put LPUART1 on the VCP (UM3115 Table 13),
 * after which the ST-LINK VCP *is* the LPUART console and no wiring is needed:
 *      PB6: SB15 + SB23 ON, SB14 + SB24 OFF
 *      PB7: SB40 + SB65 ON, SB18 + SB63 OFF
 *
 * H753: the jumper recipe has NOT been verified on that board either. Its
 * LPUART1 is on PA9/PA10, which are the pins its own VCP uses, so check its
 * user manual before assuming anything - do not reuse the line above.
 *
 * F1xx/F4xx/F7xx: LPUART not present (absent from the CMSIS headers, from
 * RM0390 and from the datasheets; hw/f{1,4,7}xx/drivers/lpuart_*.c are stubs).
 * On those parts this example is compiled empty and PRINTS NOTHING AT ALL -
 * the board looks dead, and that is expected, not a fault:
 *
 *   - STDOUT_ON_LPUART=1 routes ca_stdout_* to a peripheral that is not there,
 *     which also silences ca_os_banner() (it runs as a task through stdout);
 *   - this example sets ENABLE_DBG_UART=0 on purpose, since its whole point is
 *     to run the console on the LPUART alone - so there is no debug UART to
 *     fall back on either, and even PUT_N has no backend to write to.
 *
 * There is therefore no way to report the reason from the firmware itself.
 * Use a part with LPUART (H5xx/H7xx/U5xx) to run this example.
 ******************************************************************************
 */

#include "caos.h"

/* The note above says LPUART is absent on F1xx/F4xx/F7xx; this enforces it.
 * Without the guard the example built and ran with EN_LPUART=1, but the console
 * simply never appeared - stdout was routed to a peripheral that does not exist,
 * so there was no output at all to explain why. The drivers themselves say so:
 * hw/f{1,4,7}xx/drivers/lpuart_*.c are stubs, real only on H5/H7/U5.
 *
 * Tested on the CMSIS register block, not on a list of parts: the list of
 * 103/407/429/446/746 missed the 767, which then built the real branch and
 * stayed silent (14/10). LPUART1 exists on H5/H7/U5 and nowhere else here. */
#if !defined(LPUART1)
#define EX62_NO_LPUART_HW  1
#endif

#if EN_LPUART && STDOUT_ON_LPUART && STDIN_ON_LPUART && EN_PWR && \
    !defined(EX62_NO_LPUART_HW)

static u32 task_uptime(void *arg)
{
    (void)arg;
    u32 sec = 0;
    /* Banner printed here (task context): ca_os_configure runs pre-scheduler,
     * where ca_stdout_* has no current context (TLB=NULL).
     *
     * The delay lets ca_os_banner() finish first: it is a task too, and
     * without the wait this banner lands in the middle of it - the core
     * temperature line and the closing rule come out after ours. */
    ca_os_sleep(1000);
    EVAL_STDOUT(
        ca_stdout_put_n("\r\n=== Example 62: console LPUART low-power ===");
        ca_stdout_printf_n("LPUART1 %u 8N1 on HSI16, wake=%u",
                           LPUART_STD_BAUD, (u32)LPUART_STD_WAKE);
        ca_stdout_put_n("try: 'pwr stop 60' then press a key to wake");
    )
    while (1) {
        ca_os_sleep(15000);
        EVAL_STDOUT( ca_stdout_printf_n("ex62: uptime %u s - LPUART console alive",
                                        (sec += 15)); )
    }
    return 0;
}

void ca_os_configure(void)
{
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    EVAL_PWR( ca_pwr_init(); )                 /* register CLI "pwr" */
    EVAL_RTC( ca_rtc_init(CA_RTC_CLK_AUTO); )   /* safety wake source for stop */
}

void ca_os_main(void)
{
    ca_os_job("uptime", CA_TH_LEVEL3, task_uptime, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* no LPUART hardware, or the LPUART console flags not all enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. Here that
 * matters twice over - STDOUT_ON_LPUART routes ca_stdout_* to the very
 * peripheral this branch exists because of.
 *
 * This used to be TWO branches, the second an #error for the case where the
 * part has an LPUART but the flags are missing. That was wrong for a reason
 * that has nothing to do with ex62: CMake globs the whole tree, so a
 * compile-time failure here stops EVERY example for that MCU. It builds and
 * says so at runtime instead. */
PRIVATE_STRING(m62a, "=== Example 62 ===");
#ifdef EX62_NO_LPUART_HW
PRIVATE_STRING(m62b, "no LPUART on this part: example not supported");
#else
PRIVATE_STRING(m62b, "EN_LPUART / STDOUT_ON_LPUART / STDIN_ON_LPUART / EN_PWR disabled");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m62a);
    PUT_N(&priv_m62b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_LPUART && STDOUT_ON_LPUART && STDIN_ON_LPUART && EN_PWR */
