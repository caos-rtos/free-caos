// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex60.c
 * @brief          : Example 60 — PWM LED fade with user button
 ******************************************************************************
 * @details
 * The user LED starts on (duty DUTY_MAX). By holding down the button
 * the duty slowly decreases to DUTY_MIN, then the ramp reverses and
 * rises: while held, the LED "breathes" between the two thresholds. Releasing
 * the button freezes the brightness at the current value.
 *
 * Inversion thresholds in constants: below ~15% the LED is visually
 * off, so DUTY_MIN avoids the dead zone of the ramp.
 *
 * Supported boards (LED on timer channel, user button):
 *   F4VE             D2  = PA6  TIM3_CH1 AF2 (active low)   K0 = PE4 (low)
 *   NUCLEO-U575ZI-Q  LD2 = PB7  TIM4_CH2 AF2                B1 = PC13 (high)
 *   NUCLEO-H563ZI    LD1 = PB0  TIM3_CH3 AF2                B1 = PC13 (high)
 *   NUCLEO-H753ZI    LD1 = PB0  TIM3_CH3 AF2                B1 = PC13 (high)
 *
 * CLI: "timer info", "timer pwm <n> <ch> <duty>" for manual override.
 ******************************************************************************
 */

#include "caos.h"

/* This example drives the LED as a TIMER OUTPUT (alternate function), not as a
 * plain GPIO, so it cannot use ca_led_*: it needs the specific timer channel
 * that pin is wired to. A board without that mapping is simply out of scope -
 * EX60_OK stays 0 and the "not supported" branch at the bottom says so, rather
 * than an #error breaking the build of every other example. */
#if EN_TIMER
#define EX60_OK   1
#else
#define EX60_OK   0
#endif

/* ============================================================================
 * Board configuration
 * ========================================================================== */

#if MCU_ID == 407
  /* F4VE: LED D2 = PA6 (TIM3_CH1 AF2, active low), button K0 = PE4 (active low) */
  #define LED_PORT        CA_GPIO_PORT_A
  #define LED_PIN         CA_GPIO_PIN_6
  #define LED_AF          2U
  #define LED_ACTIVE_LOW  1
  #define PWM_TIM         3U
  #define PWM_CH          1U
  #define BTN_PORT        CA_GPIO_PORT_E
  #define BTN_PIN         CA_GPIO_PIN_4
  #define BTN_PULL        CA_GPIO_PULLUP
  #define BTN_PRESSED_LVL 0U
#elif MCU_ID == 575
  /* NUCLEO-U575ZI-Q: blue LD2 = PB7 (TIM4_CH2 AF2), button B1 = PC13 */
  #define LED_PORT        CA_GPIO_PORT_B
  #define LED_PIN         CA_GPIO_PIN_7
  #define LED_AF          2U
  #define LED_ACTIVE_LOW  0
  #define PWM_TIM         4U
  #define PWM_CH          2U
  #define BTN_PORT        CA_GPIO_PORT_C
  #define BTN_PIN         CA_GPIO_PIN_13
  #define BTN_PULL        CA_GPIO_NOPULL      /* external pulldown on board */
  #define BTN_PRESSED_LVL 1U
#elif MCU_ID == 563 || MCU_ID == 753 || MCU_ID == 746 \
   || MCU_ID == 429 || MCU_ID == 446 || MCU_ID == 767
  /* NUCLEO-144: green LD1 = PB0 (TIM3_CH3 AF2), button B1 = PC13 */
  #define LED_PORT        CA_GPIO_PORT_B
  #define LED_PIN         CA_GPIO_PIN_0
  #define LED_AF          2U
  #define LED_ACTIVE_LOW  0
  #define PWM_TIM         3U
  #define PWM_CH          3U
  #define BTN_PORT        CA_GPIO_PORT_C
  #define BTN_PIN         CA_GPIO_PIN_13
  #define BTN_PULL        CA_GPIO_NOPULL      /* external pulldown on board */
  #define BTN_PRESSED_LVL 1U
#else
  /* No LED/button mapping for this board: the example does not apply. */
  #undef  EX60_OK
  #define EX60_OK  0
#endif

#if EX60_OK

/* ============================================================================
 * Fade parameters (modifiable constants)
 * ========================================================================== */

#define PWM_FREQ_HZ   1000U   /* 1 kHz: no visible flicker */
#define DUTY_MAX      1000U   /* high inversion threshold (‰) */
#define DUTY_MIN       150U   /* low threshold: below ~15% LED is already off */
#define DUTY_STEP        5U   /* step per tick → full sweep in ~1.7 s */
#define TICK_MS         10U   /* button/ramp polling period */

static u16 s_duty = DUTY_MAX;  /* current logical duty (‰) */
static i8  s_dir  = -1;        /* -1 = descent, +1 = ascent */

/* Apply logical duty to the channel, inverting if the LED is active-low */
static void led_apply(u16 duty)
{
#if LED_ACTIVE_LOW
    ca_timer_set_pwm(PWM_TIM, PWM_CH, (u16)(DUTY_MAX - duty));
#else
    ca_timer_set_pwm(PWM_TIM, PWM_CH, duty);
#endif
}

static bool btn_pressed(void)
{
    return ca_gpio_read(BTN_PORT, BTN_PIN) == BTN_PRESSED_LVL;
}

static u32 task_fade(void *arg)
{
    (void)arg;

    /* Banner printed here (task context): ca_os_configure runs before the
     * scheduler, where ca_stdout_* has no current context (TLB=NULL). */
    EVAL_STDOUT(
        ca_stdout_put_n("\r\n=== Example 60: PWM LED fade ===");
        ca_stdout_printf_n("LED on TIM%u ch%u @ %u Hz, duty %u-%u/1000",
                           (u32)PWM_TIM, (u32)PWM_CH, PWM_FREQ_HZ,
                           DUTY_MIN, DUTY_MAX);
    )

    /* LED pin in alternate function on the timer channel */
    ca_gpio_configure(LED_PORT, LED_PIN, CA_GPIO_MODE_AF,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, LED_AF);
    /* User button as input */
    ca_gpio_configure(BTN_PORT, BTN_PIN, CA_GPIO_MODE_INPUT,
                      CA_GPIO_OTYPE_PP, CA_GPIO_SPEED_LOW, BTN_PULL, 0);

    if (!ca_timer_init_pwm(PWM_TIM, PWM_CH, PWM_FREQ_HZ)) {
        EVAL_STDOUT( ca_stdout_printf_n("ex60: init pwm TIM%u ch%u FAILED",
                                        (u32)PWM_TIM, (u32)PWM_CH); )
        return 1;
    }
    led_apply(s_duty);
    ca_timer_start(PWM_TIM);

    /* Printed here, not in the banner above: only past ca_timer_start() is the
     * LED actually driven, so an invitation to press the button is true.
     * The delay keeps it clear of the CLI prompt coming up on the same
     * stdout, which would otherwise swallow the line right after the banner. */
    ca_os_sleep(1000);
    EVAL_STDOUT(
        ca_stdout_put_n("push user button and watch the led..");
    )

    while (1) {
        ca_os_sleep(TICK_MS);
        if (!btn_pressed()) continue;

        /* Ramp in the current direction, inversion at thresholds */
        i32 d = (i32)s_duty + (i32)s_dir * (i32)DUTY_STEP;
        if (d <= (i32)DUTY_MIN) {
            d = (i32)DUTY_MIN;
            s_dir = 1;
            EVAL_STDOUT( ca_stdout_printf_n("ex60: min %u/1000, going up", (u32)d); )
        } else if (d >= (i32)DUTY_MAX) {
            d = (i32)DUTY_MAX;
            s_dir = -1;
            EVAL_STDOUT( ca_stdout_printf_n("ex60: max %u/1000, going down", (u32)d); )
        }
        s_duty = (u16)d;
        led_apply(s_duty);
    }
    return 0;
}

/* ============================================================================
 * Entry points
 * ========================================================================== */

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
    EVAL_USB_CDC_STD( ca_usb_dev_mng_cdc_cli(); )
}

void ca_os_main(void)
{
    ca_timer_init();   /* register CLI "timer" */
    ca_os_job("fade", CA_TH_LEVEL3, task_fade, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* EX60_OK == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N.
 * Two ways to get here: EN_TIMER is off, or this board has no LED wired to a
 * timer channel. */
PRIVATE_STRING(m60a, "=== Example 60 ===");
#if !EN_TIMER
PRIVATE_STRING(m60b, "EN_TIMER == 0 (skipping PWM example)");
#else
PRIVATE_STRING(m60b, "example not supported on this MCU (no LED on a timer channel)");
#endif
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m60a);
    PUT_N(&priv_m60b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EX60_OK */
