// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 30 — EXTI: interrupt from user button
 *
 * Hardware: NUCLEO-H753ZI, NUCLEO-U575ZI-Q and the STM32F4VE black board
 *
 * Pins used (no external wiring required). Only the button still differs per
 * board and is declared below; the LEDs come from objmap.h through ca_led_*,
 * so this example no longer restates a pin map that can drift out of date:
 *
 *              | H753ZI | U575ZI-Q | F4VE
 *   B1 USER    | PC13   | PC13     | PA0 (WK_UP)
 *   LED 1      | toggle on every press via EXTI
 *   LED 2      | toggle every 5 presses
 *   LED 3      | heartbeat 500 ms
 *
 * On a board that declares fewer LEDs (F407, F103 have one) the calls for the
 * missing ones return false and simply do nothing.
 *
 * Operation:
 *   At startup EXTI is DISABLED — verify first that task_poll_btn
 *   reads the button correctly (log on UART), then enable EXTI via CLI.
 *
 *   task_poll_btn : polling the B1 pin every 100 ms — logs state changes
 *   task_monitor  : logs each EXTI press, toggles LED2 every 5
 *   task_heartbeat: blink LED3 1 Hz + periodic log
 *   CLI hook      : commands exti_on / exti_off / btn
 *
 * REQUIREMENTS os_config.h:
 *   EN_EXTI  1
 *   ENABLE_CLI 1
 */

#include "caos.h"

#if EN_EXTI

#include "exti.h"

/* ============================================================================
 * HARDWARE — Nucleo-H753ZI
 * ========================================================================== */

#if MCU_ID == 407
/* STM32F4VE black board: WK_UP=PA0 (button, external pull-down -> RISING).
 * LEDs now come from objmap.h via ca_led_*. */
#define BTN_PORT        CA_GPIO_PORT_A
#define BTN_PIN         0U
#define BTN_TXT         "PA0"



#elif MCU_ID == 575
/* NUCLEO-U575ZI-Q (MB1549, UM2861 6.4): B1 USER PC13 like the H753. The LED
 * pins differ from the H753 and used to be restated here; they now live in
 * objmap.h, which is what stops a board from silently blinking nothing. */
#define BTN_PORT        CA_GPIO_PORT_C
#define BTN_PIN         13U
#define BTN_TXT         "PC13"



#else
/* NUCLEO-H753ZI (MB1364, UM2407 6.5): B1 USER PC13 */
#define BTN_PORT        CA_GPIO_PORT_C
#define BTN_PIN         13U
#define BTN_TXT         "PC13"



#endif

/* ============================================================================
 * CONFIGURATION
 * ========================================================================== */

#define DEBOUNCE_MS     50U
#define LED2_THRESHOLD  5U

/* ============================================================================
 * SHARED STATE (ISR ↔ task)
 * ========================================================================== */

static volatile u32  g_press_count = 0U;

/* Tick of the last accepted edge - the debounce reference, written in the ISR
 * and only read elsewhere. Starts one window in the past so the very first
 * press is accepted immediately. */
static volatile u32  g_last_edge_ms = (u32)0 - DEBOUNCE_MS;
static volatile bool g_exti_on     = false;   /* EXTI enabled? */

/* Raised by task_poll_btn once the button, the LEDs and the EXTI line are up.
 * The other two tasks touch the LEDs, so they wait for it instead of racing the
 * init: task creation order is not a guarantee of execution order. */
static volatile bool g_hw_ready    = false;

/* ============================================================================
 * EXTI CALLBACK — ISR context, RISING edge (press = HIGH on these boards)
 * ========================================================================== */

static void cb_button(void)
{
    /* Debounce by timestamp, not by a flag another task has to clear: the
     * bounce window is measured from THIS edge. The previous version raised a
     * flag and let task_monitor clear it DEBOUNCE_MS after it noticed - and it
     * only looks every 10 ms, so the window started late and closed early.
     * Contact bounces outliving it were counted as real presses: on the H563
     * a single press could log two or three. */
    u32 now = ca_os_tick_ms();

    if ((u32)(now - g_last_edge_ms) < DEBOUNCE_MS)
        return;                 /* still inside the bounce window */

    g_last_edge_ms = now;
    ca_led_toggle(1U);
    g_press_count++;
}

#if EN_TAMP
/* ============================================================================
 * TAMPER on the same B1 pin (paid edition)
 *
 * 'tamper arm' fills three places with a known pattern and arms the tamper;
 * a press on B1 then makes the HARDWARE erase what the family covers, before
 * any code runs. 'tamper check' says what was erased against what should be:
 *   backup registers   erased on every part
 *   backup SRAM        erased on U575, H563, H753 - KEPT on F446, F767
 *                      (RM0390/RM0410: not erased by a tamper)
 *   secret area SRAM2  erased on U575, H563; none elsewhere
 * caOS reserved SRAM2 out of the heap at boot, so the system keeps running.
 * ========================================================================== */

#define TMP_PATTERN     0xA5C3F00DU
#define TMP_BKS_WORDS   64U          /* backup SRAM: first 256 bytes */
#define TMP_SEC_WORDS   64U          /* secret area: first 256 bytes + last word */

#if MCU_ID == 575 || MCU_ID == 563 || MCU_ID == 753
#define TMP_BKS_ERASED  1
#else
#define TMP_BKS_ERASED  0
#endif

static volatile u32 g_tamper_events = 0U;

static void cb_tamper(void)          /* tamper interrupt, after the erase */
{
    g_tamper_events++;
}

static void tamper_fill(void)
{
    for (u32 i = 0U; i < ca_bkp_count(); i++) (void)ca_bkp_write(i, TMP_PATTERN ^ i);
    u32 w[TMP_BKS_WORDS];
    for (u32 i = 0U; i < TMP_BKS_WORDS; i++) w[i] = TMP_PATTERN ^ (i << 8);
    if (ca_bkp_sram_size() != 0U) (void)ca_bkp_sram_write(0U, w, sizeof(w));
    u32 ssz = 0U;
    volatile u32 *sec = (volatile u32 *)ca_memory_secret(&ssz);
    if (sec != NULL) {
        for (u32 i = 0U; i < TMP_SEC_WORDS; i++) sec[i] = TMP_PATTERN ^ (i << 16);
        sec[ssz / 4U - 1U] = TMP_PATTERN;
    }
}

/* 0 = all zero, 1 = pattern intact, 2 = something else */
static u32 state_of(u32 zero, u32 match, u32 n)
{
    return (zero == n) ? 0U : (match == n) ? 1U : 2U;
}

static const char *state_txt(u32 st)
{
    return (st == 0U) ? "erased" : (st == 1U) ? "kept" : "MIXED";
}

static void tamper_check(void)
{
    u32 zero = 0U, match = 0U, n = ca_bkp_count();
    for (u32 i = 0U; i < n; i++) {
        const u32 v = ca_bkp_read(i);
        zero += (v == 0U); match += (v == (TMP_PATTERN ^ i));
    }
    const u32 regs = state_of(zero, match, n);

    u32 bks = 3U;                                   /* 3 = not present */
    if (ca_bkp_sram_size() != 0U) {
        u32 w[TMP_BKS_WORDS];
        (void)ca_bkp_sram_read(0U, w, sizeof(w));
        zero = match = 0U;
        for (u32 i = 0U; i < TMP_BKS_WORDS; i++) {
            zero += (w[i] == 0U); match += (w[i] == (TMP_PATTERN ^ (i << 8)));
        }
        bks = state_of(zero, match, TMP_BKS_WORDS);
    }

    u32 sec = 3U, ssz = 0U;
    volatile u32 *a = (volatile u32 *)ca_memory_secret(&ssz);
    if (a != NULL) {
        zero = match = 0U;
        for (u32 i = 0U; i < TMP_SEC_WORDS; i++) {
            zero += (a[i] == 0U); match += (a[i] == (TMP_PATTERN ^ (i << 16)));
        }
        zero += (a[ssz / 4U - 1U] == 0U); match += (a[ssz / 4U - 1U] == TMP_PATTERN);
        sec = state_of(zero, match, TMP_SEC_WORDS + 1U);
    }

    CLI_PRINTF_N("EX30 TAMPER events=%u", (unsigned)g_tamper_events);
    CLI_PRINTF_N("EX30 TAMPER backup registers: %s (want erased)", state_txt(regs));
    if (bks != 3U)
        CLI_PRINTF_N("EX30 TAMPER backup SRAM: %s (want %s)", state_txt(bks),
                     TMP_BKS_ERASED ? "erased" : "kept");
    if (sec != 3U)
        CLI_PRINTF_N("EX30 TAMPER secret area %u KB at 0x%08X: %s (want erased)",
                     (unsigned)(ssz / 1024U), (u32)a, state_txt(sec));
    const bool ok = g_tamper_events != 0U && regs == 0U
                 && (bks == 3U || bks == (TMP_BKS_ERASED ? 0U : 1U))
                 && (sec == 3U || sec == 0U);
    CLI_PUT_N(ok ? "EX30 TAMPER CHECK PASS" : "EX30 TAMPER CHECK FAIL");
}

static void tamper_cli(const char *p)
{
    if (ca_strncmp(p, "arm", 3) == 0) {
        tamper_fill();
        g_tamper_events = 0U;
        if (!ca_exti_tamper_enable(cb_tamper)) {
            CLI_PUT_N("EX30 TAMPER: cannot arm (RTC not running? EN_RTC)");
            return;
        }
        u32 ssz = 0U;
        (void)ca_memory_secret(&ssz);
        CLI_PRINTF_N("EX30 TAMPER ARMED: %u regs, %u B backup SRAM, %u KB secret - press B1",
                     (unsigned)ca_bkp_count(), (unsigned)ca_bkp_sram_size(),
                     (unsigned)(ssz / 1024U));
    } else if (ca_strncmp(p, "check", 5) == 0) {
        tamper_check();
    } else if (ca_strncmp(p, "off", 3) == 0) {
        ca_exti_tamper_disable();
        CLI_PUT_N("EX30 TAMPER: off");
    } else {
        CLI_PUT_N("tamper arm | check | off");
    }
}
#endif /* EN_TAMP */

/* ============================================================================
 * CLI HOOK — user commands: exti_on / exti_off / btn
 * ========================================================================== */

static bool user_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("exti_on | exti_off | btn");
        EVAL_TAMP( CLI_PUT_N("tamper arm | check | off"); )
        return false;
    }
#if EN_TAMP
    if (ca_strncmp(p, "tamper", 6) == 0) {
        tamper_cli(ca_skip_sp(p + 6));
        return true;
    }
#endif
    if (ca_strncmp(p, "exti_on", 7) == 0) {
        /* task_poll_btn owns the EXTI setup: enabling the line before it ran
         * would arm a callback the driver has not configured yet. */
        if (!g_hw_ready) {
            CLI_PUT_N("EX30: hardware not ready yet, retry in a moment");
            return true;
        }
        g_last_edge_ms = ca_os_tick_ms() - DEBOUNCE_MS;  /* accept the next edge */
        ca_exti_enable(BTN_PIN);
        g_exti_on = true;
        CLI_PUT_N("EX30: EXTI enabled (RISING edge on the B1 pin)");
        return true;
    }
    if (ca_strncmp(p, "exti_off", 8) == 0) {
        ca_exti_disable(BTN_PIN);
        g_exti_on = false;
        CLI_PUT_N("EX30: EXTI disabled");
        return true;
    }
    if (ca_strncmp(p, "btn", 3) == 0) {
        u32 v = ca_gpio_read(BTN_PORT, BTN_PIN);
        CLI_PRINTF_N("EX30: " BTN_TXT "=%u (%s)", v, v ? "HIGH(pressed)" : "LOW(released)");
        return true;
    }
    return false;
}
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

/* ============================================================================
 * TASK POLL BTN — polling the B1 pin, logs state changes
 * ========================================================================== */

static u32 task_poll_btn(void *arg)
{
    (void)arg;

    u32 prev = 2U;   // impossible value → force log on first cycle 

    /* Hardware init lives here rather than in ca_os_configure: with a task
     * context ca_stdout_* works, so the setup and the messages that describe it
     * stay together. LEDs come from objmap.h through the driver. */
    ca_led_init();

    /* B1: input, no internal pull - the board provides the network.
     * On the U575 the button pin must NEVER be driven as OUTPUT/LOW: UM2861
     * warns it shorts the button network. Input is what we want anyway. */
    ca_gpio_configure(BTN_PORT, BTN_PIN,
                      CA_GPIO_MODE_INPUT, CA_GPIO_OTYPE_PP,
                      CA_GPIO_SPEED_LOW, CA_GPIO_NOPULL, 0U);

    /* RISING edge, but the line starts DISABLED: phase 1 is polling only. */
    ca_exti_init(BTN_PORT, BTN_PIN, CA_EXTI_EDGE_RISING, cb_button);
    ca_exti_disable(BTN_PIN);
    g_exti_on = false;

    g_hw_ready = true;

    /* Let ca_os_banner() finish first: it is a task too, at CA_TH_LEVEL6, and it
     * paces itself with sleeps to let the UART drain. This task runs at LEVEL3
     * and would otherwise print straight through the middle of it. The hardware
     * above is already up, so the other tasks are not held back by this wait. */
    ca_os_sleep(1000);

    ca_stdout_printf_n("EX30: started - EXTI OFF");

    /* Phase 1 is only half the example: polling proves the pin is read, it says
     * nothing about the EXTI path. Say so out loud, because a run that stops
     * here looks like a pass - it is how the EXTICR field-width bug survived a
     * "passed" record on the U575 (see the driver comment in exti_u5xx.c). */
    ca_stdout_printf_n("EX30: polling active on " BTN_TXT
                       " - type 'exti_on' to enable the EXTI interrupt");

    while (1) {
        u32 cur = ca_gpio_read(BTN_PORT, BTN_PIN);
        if (cur != prev) {
            prev = cur;
            if (cur == 1U)
                ca_stdout_printf_n("EX30 POLL: B1 PRESSED  (" BTN_TXT "=1)");
            else
                ca_stdout_printf_n("EX30 POLL: B1 RELEASED (" BTN_TXT "=0)");
        }
        ca_os_sleep(100);
    }
    return 0;
}

/* ============================================================================
 * TASK MONITOR — reports the EXTI count + LED2 every 5 presses
 *   (debouncing is done in the ISR by timestamp, nothing to reset here)
 * ========================================================================== */

static u32 task_monitor(void *arg)
{
    (void)arg;

    u32 last_count = 0U;
    u32 led2_group = 0U;

    while (!g_hw_ready) ca_os_sleep(10);

    while (1) {
        u32 cur = g_press_count;
        if (cur != last_count) {
            u32 delta = cur - last_count;
            last_count = cur;
            ca_stdout_printf_n("EX30 EXTI: total presses=%u (delta=%u)",
                             (unsigned)cur, (unsigned)delta);

            u32 grp = cur / LED2_THRESHOLD;
            if (grp != led2_group) {
                led2_group = grp;
                ca_led_toggle(2U);
                ca_stdout_printf_n("EX30: LED2 toggle (group %u)", (unsigned)grp);
            }
        }
        ca_os_sleep(10);
    }
    return 0;
}

/* ============================================================================
 * TASK HEARTBEAT
 * ========================================================================== */

static u32 task_heartbeat(void *arg)
{
    (void)arg;
    u32 tick = 0U;

    while (!g_hw_ready) ca_os_sleep(10);

    while (1) {
        ca_led_toggle(3U);
        ca_os_sleep(500);
        tick++;
        if ((tick % 20U) == 0U)
            ca_stdout_printf_n("EX30: uptime=%us  press=%u  exti=%s",
                             (unsigned)(tick / 2U),
                             (unsigned)g_press_count,
                             g_exti_on ? "ON" : "OFF");
    }
    return 0;
}

/* ============================================================================
 * caOS HOOK
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
        ca_cli_register(&s_user_cli_node); 
    )
    
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

    /* The button, the LEDs and the EXTI line are set up by task_poll_btn, not
     * here: this hook runs before ca_os_run, where ca_stdout_* has no task
     * context and refuses to print. Doing the init inside the task keeps the
     * hardware setup next to the messages that describe it. */
}

void ca_os_main(void)
{
   
    ca_os_job("poll_btn",   CA_TH_LEVEL3, task_poll_btn,   NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);

    ca_os_job("monitor",    CA_TH_LEVEL3, task_monitor,    NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    
    ca_os_job("heartbeat",  CA_TH_LEVEL3, task_heartbeat,  NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
  
    ca_os_run(true);
}

#else /* EN_EXTI == 0 */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m30a, "=== Example 30 ===");
PRIVATE_STRING(m30b, "EN_EXTI == 0 : no EXTI tasks created");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m30a);
    PUT_N(&priv_m30b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* EN_EXTI */
