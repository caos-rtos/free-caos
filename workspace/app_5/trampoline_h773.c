// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/*
 * trampoline_h773.c - XSPI2 trampoline of the NUCLEO-H7S3L8 (app_5, H773 only)
 *
 * Lives in the 64 KB internal flash (0x08000000). Maps the Octo-SPI flash
 * MX25UW25645G on XSPI2 (0x70000000) in single-line SPI and jumps to the
 * image whose vector table starts there. Self-contained: no caOS library,
 * no CMSIS startup, no kernel - registers only (built by
 * workspace/h7rsxx/h773/CMakeLists.txt with trampoline_h773.ld).
 *
 * Boot:
 *   1. HSI 64 MHz, no PLL (the image starts as after a reset).
 *   2. PWR_CSR2: LDO supply + EN_XSPIM2 in ONE write. The low byte of
 *      PWR_CSR2 is write-once after POR (RM0477 6.8.4) and its reset value
 *      0x06 is an ILLEGAL combination (Table 44): a read-modify-write of
 *      EN_XSPIM2 alone would lock it and the image could never raise VOS.
 *      The image (clock_h7rsxx.c) finds LDO already set and leaves it.
 *   3. XSPIM direct mode, port N on AF9, XSPI2 kernel clock = hclk5.
 *   4. Flash software reset in 8D, 8S and 1S (0x66/0x99), because a reset of
 *      the MCU alone can leave the flash in OPI; then Read ID 0x9F = C2 81 39.
 *   5. Memory-mapped single line, 0x0C (4-byte fast read), 8 dummy cycles;
 *      then octal DTR (WRCR2 DOPI, 0xEE11, 20 dummy, DQS), kept only if the
 *      first 64 words read the same as in SPI - otherwise back to SPI.
 *      With the XSPI2_HSLV option byte: HSLV I/O + compensation cell, DTR
 *      checked at 64 MHz, then /4 = 75 MHz once the image raises hclk5 to
 *      300 MHz. Without it: /8 = 37.5 MHz. The image never reprograms XSPI2.
 *   6. Vector check: SP in DTCM/AXI SRAM, Reset in XSPI2 with the Thumb bit.
 *   7. USART3 reset, VTOR, MSP, cpsie i, jump (as ca_os_dead does).
 *
 * Monitor (USART3 115200 = ST-LINK VCP, same syntax as the caOS CLI):
 *   mem AAAAAAAA [VVVVVVVV]   read / write one word
 *   hexdump AAAAAAAA [N]      N lines of 16 bytes (default 4, max 32)
 *   id                        JEDEC ID of the XSPI2 flash
 *   go [AAAAAAAA]             jump to the image (default 0x70000000)
 *   reset                     system reset
 * Entered with B2 (PC13, active LOW) held at reset, or by itself when the
 * flash does not answer or holds no image (red LED blinking). A bus fault
 * from mem/hexdump prints CFSR/BFAR and returns to the prompt.
 */

#include "caos.h"

#if MCU_ID == 773

#define TRAMP_VERSION   "1.3"

#define IMG_BASE        0x70000000UL      /* XSPI2 memory-mapped window */
#define IMG_SIZE        0x02000000UL      /* 32 MB */

#define HSI_HZ          64000000UL
#define CONSOLE_BAUD    115200UL

#define XSPI_PRESC      7U                /* kernel / 8: run clock without HSLV */
#define XSPI_PRESC_HSLV 3U                /* kernel / 4: run clock with HSLV (75 MHz at 300) */
/* Kept at /4. Measured 02/11/2026 ('e92 xspeed', this board, room temp.):
 * with the clocks already up, reading 3 x 512 KB of the image from RAM with
 * the IRQs off gives the CRC of 75 MHz at /3 = 100 MHz and /2 = 150 MHz, 6
 * runs out of 6. But a trampoline programming /3 or /2 does NOT boot: the
 * image hangs right after the jump. clock_h7rsxx.c switches SYSCLK to PLL1
 * executing from XSPI2, and the XSPI2 kernel (hclk5) jumps from 32 to
 * 300 MHz under a fetch in flight: from 8 to 75 MHz at /4 it survives, to
 * 100 or 150 MHz it does not. Faster needs that switch run from RAM with
 * XSPI2 idle, then a new check. 300 MHz (/1) never: the MX25UW25645G stops
 * at 200 MHz DTR. */
#define XSPI_PRESC_CHK  0U                /* kernel / 1: DTR check at boot (64 MHz on HSI) */
#define FLASH_ID        0x3981C2UL        /* MX25UW25645G: C2 81 39 (read on the board) */

/* ============================================================================
 * Startup (no CMSIS startup: vectors and Reset_Handler are here)
 * ========================================================================== */

extern u32 _estack, _sidata, _sdata, _edata, _sbss, _ebss;
int  main(void);
void Reset_Handler(void);
void Fault_Handler(void);

__attribute__((section(".isr_vector"), used))
static void (* const s_vectors[16])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    Fault_Handler,          /* NMI */
    Fault_Handler,          /* HardFault */
    Fault_Handler,          /* MemManage */
    Fault_Handler,          /* BusFault */
    Fault_Handler,          /* UsageFault */
    0, 0, 0, 0,
    Fault_Handler,          /* SVC */
    Fault_Handler,          /* DebugMon */
    0,
    Fault_Handler,          /* PendSV */
    Fault_Handler,          /* SysTick */
};

void Reset_Handler(void)
{
    u32 *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0U;

    SCB->VTOR = (u32)s_vectors;
    SCB->CPACR |= (3UL << 20U) | (3UL << 22U);   /* CP10/CP11: no FP code, but safe */
    __DSB(); __ISB();
    (void)main();
    for (;;) { }
}

/* ============================================================================
 * Console: USART3 PD8 (TX) / PD9 (RX) AF7, polled, pclk1 = HSI 64 MHz
 * ========================================================================== */

static void gpio_af(GPIO_TypeDef *g, u32 pin, u32 af, u32 speed)
{
    g->MODER   = (g->MODER   & ~(3U << (pin * 2U))) | (2U << (pin * 2U));
    g->OSPEEDR = (g->OSPEEDR & ~(3U << (pin * 2U))) | (speed << (pin * 2U));
    g->PUPDR  &= ~(3U << (pin * 2U));
    volatile u32 *afr = &g->AFR[pin >> 3];
    *afr = (*afr & ~(0xFU << ((pin & 7U) * 4U))) | (af << ((pin & 7U) * 4U));
}

static void con_init(void)
{
    RCC->AHB4ENR  |= RCC_AHB4ENR_GPIODEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_USART3EN;
    (void)RCC->APB1ENR1;
    gpio_af(GPIOD, 8U, 7U, 1U);
    gpio_af(GPIOD, 9U, 7U, 1U);
    USART3->CR1 = 0U;
    USART3->BRR = (HSI_HZ + CONSOLE_BAUD / 2U) / CONSOLE_BAUD;
    USART3->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void con_putc(char c)
{
    while (!(USART3->ISR & USART_ISR_TXE_TXFNF)) { }
    USART3->TDR = (u32)(u8)c;
}

static void con_puts(const char *s)
{
    while (*s) con_putc(*s++);
}

static void con_putln(const char *s)
{
    con_puts(s);
    con_puts("\r\n");
}

static void con_hex(u32 v, u32 digits)
{
    static const char hx[] = "0123456789ABCDEF";
    while (digits--) con_putc(hx[(v >> (digits * 4U)) & 0xFU]);
}

static void con_hex_lc(u32 v, u32 digits)
{
    static const char hx[] = "0123456789abcdef";
    while (digits--) con_putc(hx[(v >> (digits * 4U)) & 0xFU]);
}

static bool con_getc(char *c)
{
    if (USART3->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE))
        USART3->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    if (!(USART3->ISR & USART_ISR_RXNE_RXFNE)) return false;
    *c = (char)USART3->RDR;
    return true;
}

static void con_flush(void)
{
    while (!(USART3->ISR & USART_ISR_TC)) { }
}

/* ============================================================================
 * Board: LD3 red PB7 (active high), B2 PC13 (active LOW, internal pull-up)
 * ========================================================================== */

static void delay_loops(u32 n)
{
    for (volatile u32 i = n; i; i--) { }
}

static void board_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOBEN | RCC_AHB4ENR_GPIOCEN;
    (void)RCC->AHB4ENR;
    GPIOB->MODER = (GPIOB->MODER & ~(3U << (7U * 2U))) | (1U << (7U * 2U));
    GPIOB->BSRR  = (1U << (7U + 16U));
    /* B2 on PC13: the board has NO external pull-up (measured 01/11/2026: the
     * pin follows the internal pull), so the internal pull-up is what makes
     * "released = 1, pressed = 0" (UM3276 7.6). Settle time before the read. */
    GPIOC->MODER &= ~(3U << (13U * 2U));                    /* input */
    GPIOC->PUPDR  = (GPIOC->PUPDR & ~(3U << (13U * 2U))) | (1U << (13U * 2U));
    delay_loops(10000U);
}

/* PC13 is also TAMP_IN1, and a backup-domain pin is driven by the TAMP
 * "whatever the GPIO configuration" (RM0477 49.3.3): with tamper 1 armed - it
 * survives a system reset - the pull-up above does not apply, PC13 reads low
 * and every reset stopped here as if B2 were held (02/11, ex30). Then B2 is
 * not trusted: the image runs and decides about the tamper itself; a broken
 * image still lands in the monitor. Reading TAMP needs RTCAPBEN and DBP (with
 * DBP = 0 the backup domain reads 0). */
static bool pc13_owned_by_tamp(void)
{
    RCC->APB4ENR |= RCC_APB4ENR_RTCAPBEN;
    (void)RCC->APB4ENR;
    PWR->CR1 |= PWR_CR1_DBP;
    __DSB();
    return (TAMP->CR1 & TAMP_CR1_TAMP1E) != 0U;
}

static bool b2_pressed(void)
{
    if (pc13_owned_by_tamp()) { con_putln("B2 ignored: PC13 is armed as TAMP_IN1"); return false; }
    return (GPIOC->IDR & (1U << 13U)) == 0U;
}
static void led_red(bool on)  { GPIOB->BSRR = on ? (1U << 7U) : (1U << (7U + 16U)); }


/* ============================================================================
 * XSPI2 + MX25UW25645G
 * ========================================================================== */

#define CCR_IMODE(m)    ((u32)(m) << XSPI_CCR_IMODE_Pos)
#define CCR_ISIZE(s)    ((u32)(s) << XSPI_CCR_ISIZE_Pos)
#define CCR_ADMODE(m)   ((u32)(m) << XSPI_CCR_ADMODE_Pos)
#define CCR_ADSIZE(s)   ((u32)(s) << XSPI_CCR_ADSIZE_Pos)
#define CCR_DMODE(m)    ((u32)(m) << XSPI_CCR_DMODE_Pos)
#define CR_FMODE(m)     ((u32)(m) << XSPI_CR_FMODE_Pos)

#define LINES_1         1U
#define LINES_8         4U
#define FMODE_IWRITE    0U
#define FMODE_IREAD     1U
#define FMODE_MMAP      3U

static bool xspi_idle(void)
{
    for (u32 n = 1000000U; n; n--)
        if (!(XSPI2->SR & XSPI_SR_BUSY)) return true;
    return false;
}

static bool xspi_wait_tc(void)
{
    for (u32 n = 1000000U; n; n--) {
        if (XSPI2->SR & XSPI_SR_TCF) { XSPI2->FCR = XSPI_FCR_CTCF; return true; }
    }
    return false;
}

static void xspi_pins(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIONEN;
    (void)RCC->AHB4ENR;
    for (u32 pin = 0U; pin <= 11U; pin++)                  /* DQS0 NCS1 IO0..7 CLK NCLK */
        gpio_af(GPION, pin, 9U, 3U);
}

/* XSPIM_P2 high-speed low-voltage I/O. Allowed: the port is supplied by
 * VDDXSPI2 = 1V8 through SB34 (MB1737 schematic p.4), and DS14359 Table 23
 * limits HSLV by the supply of the I/O domain (VDD, VDDXSPI1 or VDDXSPI2), not
 * by VDD alone - ST's own XSPI figures run a 3 V MCU with 1.8 V XSPI rails in
 * HSLV. Effective only with the XSPI2_HSLV option byte set (set on this board
 * on 01/11/2026, reversible with STM32CubeProgrammer -ob XSPI2_HSLV=0).
 * NEVER for port 1: VDDXSPI1 = VDD = 3.3 V on this board. */
static bool xspi_hslv_on(void)
{
    if (!(FLASH->OBW1SR & FLASH_OBW1SR_XSPI2_HSLV)) return false;
    RCC->APB4ENR |= RCC_APB4ENR_SBSEN;
    (void)RCC->APB4ENR;
    SBS->CCCSR |= SBS_CCCSR_XSPI2_IOHSLV | SBS_CCCSR_XSPI2_COMP_EN;
    for (u32 n = 1000000U; n; n--)
        if (SBS->CCCSR & SBS_CCCSR_XSPI2_COMP_RDY) return true;
    return true;    /* compensation not ready: HSLV still on, the DTR check decides */
}

static void xspi_set_presc(u32 presc)
{
    (void)xspi_idle();
    XSPI2->CR  &= ~XSPI_CR_EN;
    XSPI2->DCR2 = (presc << XSPI_DCR2_PRESCALER_Pos);
    XSPI2->CR  |= XSPI_CR_EN;
}

static void xspi_hw_init(void)
{
    /* LDO supply + XSPIM_P2 enabled, one write (see the header) */
    u32 csr2 = PWR->CSR2;
    csr2 &= ~(PWR_CSR2_SDHILEVEL | PWR_CSR2_SMPSEXTHP | PWR_CSR2_SDEN | PWR_CSR2_BYPASS);
    csr2 |= PWR_CSR2_LDOEN | PWR_CSR2_EN_XSPIM2;
    PWR->CSR2 = csr2;
    for (u32 n = 1000000U; n && !(PWR->SR1 & PWR_SR1_ACTVOSRDY); n--) { }

    RCC->AHB5ENR |= RCC_AHB5ENR_XSPI2EN | RCC_AHB5ENR_XSPIMEN;
    (void)RCC->AHB5ENR;
    RCC->CCIPR1 &= ~RCC_CCIPR1_XSPI2SEL_Msk;               /* 00 = hclk5 (reset value) */
    XSPIM->CR = 0U;                                         /* direct: XSPI2 -> port 2 */
    xspi_pins();

    XSPI2->CR   = 0U;
    XSPI2->DCR1 = (1U << XSPI_DCR1_MTYP_Pos)               /* Macronix */
                | (24U << XSPI_DCR1_DEVSIZE_Pos)           /* 2^(24+1) = 32 MB */
                | (3U << XSPI_DCR1_CSHT_Pos);              /* CS high >= 4 cycles */
    XSPI2->DCR2 = (XSPI_PRESC << XSPI_DCR2_PRESCALER_Pos);
    XSPI2->CR   = XSPI_CR_EN;
}

/* Command only (no address, no data): starts when IR is written */
static bool xspi_cmd(u32 ccr, u32 ir)
{
    if (!xspi_idle()) return false;
    XSPI2->CR  = (XSPI2->CR & ~XSPI_CR_FMODE_Msk) | CR_FMODE(FMODE_IWRITE);
    XSPI2->CCR = ccr;
    XSPI2->TCR = 0U;
    XSPI2->IR  = ir;
    return xspi_wait_tc();
}

static void flash_reset_all_modes(void)
{
    const u32 dtr8 = CCR_IMODE(LINES_8) | XSPI_CCR_IDTR | CCR_ISIZE(1U);   /* 16-bit cmd */
    const u32 str8 = CCR_IMODE(LINES_8) | CCR_ISIZE(1U);
    const u32 spi1 = CCR_IMODE(LINES_1) | CCR_ISIZE(0U);
    (void)xspi_cmd(dtr8, 0x6699U); (void)xspi_cmd(dtr8, 0x9966U); delay_loops(200000U);
    (void)xspi_cmd(str8, 0x6699U); (void)xspi_cmd(str8, 0x9966U); delay_loops(200000U);
    (void)xspi_cmd(spi1, 0x66U);   (void)xspi_cmd(spi1, 0x99U);   delay_loops(200000U);
}

static u32 flash_read_id(void)
{
    if (!xspi_idle()) return 0U;
    XSPI2->CR  = (XSPI2->CR & ~XSPI_CR_FMODE_Msk) | CR_FMODE(FMODE_IREAD);
    XSPI2->DLR = 3U - 1U;
    XSPI2->CCR = CCR_IMODE(LINES_1) | CCR_ISIZE(0U) | CCR_DMODE(LINES_1);
    XSPI2->TCR = 0U;
    XSPI2->IR  = 0x9FU;
    u32 id = 0U;
    for (u32 i = 0U; i < 3U; i++) {
        u32 n = 1000000U;
        while (!(XSPI2->SR & (XSPI_SR_FTF | XSPI_SR_TCF)) && --n) { }
        if (!n) return 0U;
        id |= (u32)(*(volatile u8 *)&XSPI2->DR) << (i * 8U);
    }
    (void)xspi_wait_tc();
    return id;
}

/* Memory-mapped read, single line: FAST READ 4B (0x0C), 8 dummy cycles */
static bool xspi_mmap_spi(void)
{
    if (!xspi_idle()) return false;
    XSPI2->CR  = (XSPI2->CR & ~XSPI_CR_FMODE_Msk) | CR_FMODE(FMODE_MMAP);
    XSPI2->CCR = CCR_IMODE(LINES_1) | CCR_ISIZE(0U)
               | CCR_ADMODE(LINES_1) | CCR_ADSIZE(3U)      /* 32-bit address */
               | CCR_DMODE(LINES_1);
    XSPI2->TCR = (8U << XSPI_TCR_DCYC_Pos);
    XSPI2->IR  = 0x0CU;
    return true;
}

/* Memory-mapped read, octal DTR (8D-8D-8D): 8DTRD 0xEE11, 32-bit address,
 * 20 dummy cycles (CR2 0x300 at its reset value), data strobe (DQS), Macronix
 * byte order (DCR1.MTYP). Half-cycle hold on the outputs as DTR wants. */
static bool xspi_mmap_dtr(void)
{
    if (!xspi_idle()) return false;
    XSPI2->CR  = (XSPI2->CR & ~XSPI_CR_FMODE_Msk) | CR_FMODE(FMODE_MMAP);
    XSPI2->CCR = CCR_IMODE(LINES_8) | XSPI_CCR_IDTR | CCR_ISIZE(1U)
               | CCR_ADMODE(LINES_8) | XSPI_CCR_ADDTR | CCR_ADSIZE(3U)
               | CCR_DMODE(LINES_8) | XSPI_CCR_DDTR | XSPI_CCR_DQSE;
    XSPI2->TCR = (20U << XSPI_TCR_DCYC_Pos) | XSPI_TCR_DHQC;
    XSPI2->IR  = 0xEE11U;
    return true;
}

/* Leaves memory-mapped mode: ABORT, then EN stays on for indirect commands */
static void xspi_unmap(void)
{
    XSPI2->CR |= XSPI_CR_ABORT;
    for (u32 n = 1000000U; n && (XSPI2->CR & XSPI_CR_ABORT); n--) { }
    (void)xspi_idle();
}

/* SPI mode: WREN, then WRCR2 (0x72) addr 0x00000000 <- 0x02 = DOPI (octal DTR).
 * From the last clock of this command on, the flash talks 8D-8D-8D only. */
static bool flash_enter_dopi(void)
{
    if (!xspi_cmd(CCR_IMODE(LINES_1) | CCR_ISIZE(0U), 0x06U)) return false;
    if (!xspi_idle()) return false;
    XSPI2->CR  = (XSPI2->CR & ~XSPI_CR_FMODE_Msk) | CR_FMODE(FMODE_IWRITE);
    XSPI2->DLR = 1U - 1U;
    XSPI2->CCR = CCR_IMODE(LINES_1) | CCR_ISIZE(0U)
               | CCR_ADMODE(LINES_1) | CCR_ADSIZE(3U) | CCR_DMODE(LINES_1);
    XSPI2->TCR = 0U;
    XSPI2->IR  = 0x72U;
    XSPI2->AR  = 0x00000000U;
    *(volatile u8 *)&XSPI2->DR = 0x02U;
    return xspi_wait_tc();
}

static bool s_mapped, s_dtr;

/* Words compared between the SPI and the DTR mapping before trusting DTR */
#define DTR_CHECK_WORDS  64U

static u32 xspi_sum(void)
{
    u32 sum = 0U;
    for (u32 i = 0U; i < DTR_CHECK_WORDS; i++)
        sum = (sum << 1 | sum >> 31) ^ ((volatile u32 *)IMG_BASE)[i];
    return sum;
}

/* From a flash in SPI mode, mapped in SPI: try octal DTR, keep it only if
 * the first words read the same. Returns true when DTR is in use. */
static bool xspi_try_dtr(void)
{
    const u32 ref = xspi_sum();
    xspi_unmap();
    if (flash_enter_dopi() && xspi_mmap_dtr() && xspi_sum() == ref)
        return true;
    xspi_unmap();
    flash_reset_all_modes();            /* back to SPI whatever state DOPI left */
    (void)xspi_mmap_spi();
    return false;
}

static u32 flash_id_cmd(void)
{
    if (s_mapped) xspi_unmap();
    flash_reset_all_modes();            /* the ID is read in SPI */
    u32 id = flash_read_id();
    s_mapped = xspi_mmap_spi();
    if (s_mapped && s_dtr) s_dtr = xspi_try_dtr();
    return id;
}

/* ============================================================================
 * Image check and jump
 * ========================================================================== */

static const char *image_check(u32 base)
{
    const u32 sp = *(volatile u32 *)base;
    const u32 pc = *(volatile u32 *)(base + 4U);
    if (sp == 0xFFFFFFFFU && pc == 0xFFFFFFFFU)            return "erased (no image)";
    if (!((sp >= 0x20000000U && sp <= 0x20030000U) ||
          (sp >= 0x24000000U && sp <= 0x24072000U)))       return "stack pointer not in DTCM/AXI SRAM";
    if (!(pc & 1U))                                        return "reset vector without the Thumb bit";
    if (pc < IMG_BASE || pc >= IMG_BASE + IMG_SIZE)        return "reset vector outside XSPI2";
    return NULL;
}

__attribute__((noreturn)) static void image_jump(u32 base)
{
    const u32 sp = *(volatile u32 *)base;
    const u32 pc = *(volatile u32 *)(base + 4U);

    con_putln("-> jump");
    con_flush();
    __disable_irq();
    SysTick->CTRL = 0U;
    SCB->SHCSR &= ~(SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk);
    /* the console back to reset: caOS reprograms USART3 from scratch */
    RCC->APB1RSTR1 |=  RCC_APB1RSTR1_USART3RST;
    RCC->APB1RSTR1 &= ~RCC_APB1RSTR1_USART3RST;
    RCC->APB1ENR1  &= ~RCC_APB1ENR1_USART3EN;
    led_red(false);

    SCB_EnableICache();                 /* the image fetches from XSPI2 from now on */
    SCB->VTOR = base;
    __DSB(); __ISB();
    __asm volatile ("msr msp, %0 \n"
                    "cpsie i     \n"
                    "bx  %1      \n" : : "r"(sp), "r"(pc) : "memory");
    __builtin_unreachable();
}

/* ============================================================================
 * Fault handling: recoverable while a monitor access is in flight
 * ========================================================================== */

static volatile bool s_probing, s_faulted;
static volatile u32  s_cfsr, s_bfar, s_hfsr;

__attribute__((used)) static void fault_c(u32 *frame)
{
    s_cfsr = SCB->CFSR;
    s_bfar = SCB->BFAR;
    s_hfsr = SCB->HFSR;
    SCB->CFSR = s_cfsr;                 /* write-1-to-clear */
    SCB->HFSR = s_hfsr;
    if (s_probing) {
        s_faulted = true;
        /* precise: skip the faulting load/store; imprecise: the PC is
         * already past the store, nothing to skip */
        if (!(s_cfsr & SCB_CFSR_IMPRECISERR_Msk)) {
            const u16 op = *(const u16 *)frame[6];
            frame[6] += ((op & 0xF800U) >= 0xE800U) ? 4U : 2U;
        }
        return;
    }
    con_puts("\r\nTRAMPOLINE FAULT CFSR=0x"); con_hex(s_cfsr, 8U);
    con_puts(" BFAR=0x"); con_hex(s_bfar, 8U);
    con_puts(" PC=0x");   con_hex(frame[6], 8U);
    con_putln("");
    for (;;) { led_red(true); delay_loops(2000000U); led_red(false); delay_loops(2000000U); }
}

__attribute__((naked)) void Fault_Handler(void)
{
    __asm volatile ("tst lr, #4      \n"
                    "ite eq          \n"
                    "mrseq r0, msp   \n"
                    "mrsne r0, psp   \n"
                    "b fault_c       \n");
}

__attribute__((noinline)) static bool peek(u32 addr, u32 *val)
{
    s_faulted = false; s_probing = true;
    __DSB();
    u32 v = *(volatile u32 *)addr;
    __DSB();
    s_probing = false;
    *val = v;
    return !s_faulted;
}

__attribute__((noinline)) static bool poke(u32 addr, u32 val)
{
    s_faulted = false; s_probing = true;
    __DSB();
    *(volatile u32 *)addr = val;
    __DSB();                            /* an imprecise fault lands before this returns */
    s_probing = false;
    return !s_faulted;
}

static void print_fault(u32 addr)
{
    con_puts("mem: bus fault at 0x"); con_hex(addr, 8U);
    con_puts(" (CFSR=0x"); con_hex(s_cfsr, 8U);
    con_puts(" BFAR=0x");  con_hex(s_bfar, 8U);
    con_putln(")");
}

/* ============================================================================
 * Monitor
 * ========================================================================== */

#define LINE_MAX  80U

static const char *skip_blank(const char *p) { while (*p == ' ') p++; return p; }

static bool parse_hex(const char **pp, u32 *out)
{
    const char *p = skip_blank(*pp);
    u32 v = 0U, n = 0U;
    for (;; p++, n++) {
        char c = *p;
        if      (c >= '0' && c <= '9') v = (v << 4) | (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (u32)(c - 'A' + 10);
        else break;
    }
    *pp = p;
    if (n) *out = v;                    /* no digits: the caller's default stays */
    return n > 0U;
}

static bool parse_dec(const char **pp, u32 *out)
{
    const char *p = skip_blank(*pp);
    u32 v = 0U, n = 0U;
    for (; *p >= '0' && *p <= '9'; p++, n++) v = v * 10U + (u32)(*p - '0');
    *pp = p;
    if (n) *out = v;
    return n > 0U;
}

static bool word_is(const char *p, const char *w)
{
    while (*w) if (*p++ != *w++) return false;
    return *p == '\0' || *p == ' ';
}

static void cmd_mem(const char *p)
{
    u32 addr, val, before;
    if (!parse_hex(&p, &addr)) { con_putln("mem: AAAAAAAA [VVVVVVVV]"); return; }
    if (addr & 3U) {
        con_puts("mem: address 0x"); con_hex(addr, 8U); con_putln(" not 4-byte aligned");
        return;
    }
    if (parse_hex(&p, &val)) {
        if (!peek(addr, &before) || !poke(addr, val) || !peek(addr, &val)) { print_fault(addr); return; }
        con_puts("mem: [0x"); con_hex(addr, 8U); con_puts("] 0x"); con_hex(before, 8U);
        con_puts(" -> 0x");   con_hex(val, 8U);  con_putln("");
    } else {
        if (!peek(addr, &val)) { print_fault(addr); return; }
        con_puts("mem: [0x"); con_hex(addr, 8U); con_puts("] = 0x"); con_hex(val, 8U); con_putln("");
    }
}

/* Same layout as ca_hexdump(): "0xAAAAAAAA  hh hh ... |ascii|" */
static void cmd_hexdump(const char *p)
{
    u32 addr, lines = 4U;
    if (!parse_hex(&p, &addr)) { con_putln("hexdump: AAAAAAAA [N]"); return; }
    if (parse_dec(&p, &lines)) {
        if (lines == 0U) lines = 4U;
        if (lines > 32U) lines = 32U;
    }
    addr &= ~3U;
    for (u32 l = 0U; l < lines; l++, addr += 16U) {
        u8 b[16];
        for (u32 w = 0U; w < 4U; w++) {
            u32 v;
            if (!peek(addr + w * 4U, &v)) { print_fault(addr + w * 4U); return; }
            b[w * 4U + 0U] = (u8)v;         b[w * 4U + 1U] = (u8)(v >> 8);
            b[w * 4U + 2U] = (u8)(v >> 16); b[w * 4U + 3U] = (u8)(v >> 24);
        }
        con_puts("0x"); con_hex(addr, 8U); con_puts("  ");
        for (u32 i = 0U; i < 16U; i++) { con_hex_lc(b[i], 2U); con_putc(' '); }
        con_putc('|');
        for (u32 i = 0U; i < 16U; i++) con_putc((b[i] >= 32U && b[i] <= 126U) ? (char)b[i] : '.');
        con_putln("|");
    }
}

static void cmd_go(const char *p)
{
    u32 base = IMG_BASE;
    (void)parse_hex(&p, &base);
    const char *why = image_check(base);
    if (why) { con_puts("go: "); con_putln(why); return; }
    image_jump(base);
}

static void cmd_help(void)
{
    con_putln("mem AAAAAAAA [VVVVVVVV]  read/write a word");
    con_putln("hexdump AAAAAAAA [N]     N lines of 16 bytes (max 32)");
    con_putln("id                       XSPI2 flash JEDEC ID");
    con_putln("go [AAAAAAAA]            jump to the image (default 70000000)");
    con_putln("reset                    system reset");
}

static void monitor_exec(const char *line)
{
    const char *p = skip_blank(line);
    if (*p == '\0') return;
    if      (word_is(p, "mem"))     cmd_mem(p + 3);
    else if (word_is(p, "hexdump")) cmd_hexdump(p + 7);
    else if (word_is(p, "id"))      { u32 id = flash_id_cmd(); con_puts("id: 0x"); con_hex(id, 6U); con_putln(id == FLASH_ID ? " (MX25UW25645G)" : " (unexpected)"); }
    else if (word_is(p, "go"))      cmd_go(p + 2);
    else if (word_is(p, "reset"))   { con_flush(); NVIC_SystemReset(); }
    else if (word_is(p, "help"))    cmd_help();
    else                            { con_puts("unknown command: "); con_putln(p); }
}

__attribute__((noreturn)) static void monitor(bool blink)
{
    char line[LINE_MAX];
    u32 len = 0U, tick = 0U;
    bool led = false;

    con_putln("monitor: 'help' for the commands");
    con_puts("> ");
    for (;;) {
        char c;
        if (blink && ++tick >= 400000U) { tick = 0U; led = !led; led_red(led); }
        if (!con_getc(&c)) continue;
        if (c == '\r' || c == '\n') {
            con_puts("\r\n");
            line[len] = '\0';
            monitor_exec(line);
            len = 0U;
            con_puts("> ");
        } else if ((c == '\b' || c == 0x7F) && len) {
            len--;
            con_puts("\b \b");
        } else if (c >= ' ' && len < LINE_MAX - 1U) {
            line[len++] = c;
            con_putc(c);
        }
    }
}

/* ============================================================================
 * main
 * ========================================================================== */

int main(void)
{
    board_init();
    con_init();
    SCB->SHCSR |= SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;

    con_putln("");
    con_putln("caOS trampoline H773 " TRAMP_VERSION " (XSPI2 -> 0x70000000)");

    xspi_hw_init();
    flash_reset_all_modes();
    const u32 id = flash_read_id();
    con_puts("XSPI2 flash ID 0x"); con_hex(id, 6U);
    if (id != FLASH_ID) {
        con_putln(" - flash does not answer");
        monitor(true);
    }
    con_putln(" (MX25UW25645G)");

    /* With HSLV the DTR check runs at 64 MHz (kernel / 1 on the HSI), just
     * under the run clock of 75 MHz (hclk5 300 MHz / 4, see XSPI_PRESC_HSLV);
     * the I/Os are rated far above both with HSLV.
     * Without HSLV everything stays at kernel / 8. */
    const bool hslv = xspi_hslv_on();
    if (hslv) xspi_set_presc(XSPI_PRESC_CHK);

    s_mapped = xspi_mmap_spi();
    if (!s_mapped) { con_putln("XSPI2: memory-mapped mode failed"); monitor(true); }
    s_dtr = xspi_try_dtr();

    if (hslv) {                         /* run clock for the image */
        xspi_unmap();
        xspi_set_presc(s_dtr ? XSPI_PRESC_HSLV : XSPI_PRESC);
        s_mapped = s_dtr ? xspi_mmap_dtr() : xspi_mmap_spi();
    }
    if (s_dtr)
        con_putln(hslv ? "XSPI2: HSLV, octal DTR checked at 64 MHz, run clock hclk5/4"
                       : "XSPI2: octal DTR (8D-8D-8D, DQS), clock hclk5/8");
    else
        con_putln("XSPI2: octal DTR check FAILED, staying in single-line SPI");

    const char *why = image_check(IMG_BASE);
    con_puts("image SP=0x"); con_hex(*(volatile u32 *)IMG_BASE, 8U);
    con_puts(" PC=0x");      con_hex(*(volatile u32 *)(IMG_BASE + 4U), 8U);
    con_putln("");
    if (why)          { con_puts("image: "); con_putln(why); monitor(true); }
    if (b2_pressed()) { con_putln("B2 held: monitor"); monitor(false); }

    image_jump(IMG_BASE);
}

#else
/* the other boards build ca_os_app_5.c alone: keep this unit non-empty */
typedef int trampoline_h773_unused_t;
#endif /* MCU_ID == 773 */
