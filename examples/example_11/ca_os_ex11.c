// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com
/**
 * @file    ca_os_ex11.c
 * @brief   Test 11: Core Temperature Monitor with Watchdog Protection
 * @details This test demonstrates how to use the internal temperature sensor
 *          to monitor core temperature and protect the system from
 *          overheating by reducing the clock or stopping the watchdog.
 *
 * The test performs the following operations:
 * 1. Initializes the internal temperature sensor
 * 2. Initializes the watchdog timer for system protection
 * 3. Creates a task that monitors the temperature every 500ms
 * 4. If the temperature exceeds the thresholds:
 *    - 75C: reduces the core clock to 50%
 *    - 85C: stops WDT reload → system reset after ~2s
 * 5. If the temperature drops below 65C, restores normal configuration
 *
 * @note The temperature is read via internal ADC (temperature sensor channel)
 * @note The watchdog protects against software hangs during normal operation
 */

#include "caos.h"
#include "temp_sensor.h"

#if ENABLE_SENSOR && EN_WDT

/* =============================================================================
 * TEST CONFIGURATION
 * ============================================================================= */

/**
 * @brief Temperature threshold for clock reduction in degrees Celsius
 * @details When the temperature exceeds this value, the clock is halved
 */
#define TEMP_CLOCK_THRESHOLD      75      // 75C - clock reduction threshold

/**
 * @brief Temperature threshold for WDT reset in degrees Celsius
 * @details When the temperature exceeds this value, the WDT is no longer reloaded
 */
#define TEMP_WDT_RESET_THRESHOLD  85      // 85C - WDT reset threshold

/**
 * @brief Recovery temperature threshold in degrees Celsius
 * @details When the temperature drops below this value, the system returns to normal
 * @details 10C hysteresis to avoid frequent oscillations
 */
#define TEMP_RECOVERY_THRESHOLD   65      // 65C - recovery threshold

/**
 * @brief Temperature reading period in milliseconds
 */
#define TEMP_READ_PERIOD_MS       500     // 500ms

/**
 * @brief Watchdog timeout in milliseconds
 */
#define WDT_TIMEOUT_MS            2000    // 2 seconds

/**
 * @brief Type of watchdog to use
 */
#define WDT_TYPE_USED             CA_WDT_TYPE_IWDG

/**
 * @brief Watchdog mode
 */
#define WDT_MODE_USED             CA_WDT_MODE_RESET

/* =============================================================================
 * GLOBAL VARIABLES
 * ============================================================================= */

/**
 * @brief Thermal protection system state
 */
typedef enum {
    THERM_STATE_NORMAL = 0,           // Normal operation (T < 65C)
    THERM_STATE_CLOCK_REDUCE,         // Reduced clock (75C <= T < 85C)
    THERM_STATE_WDT_RESET,            // WDT stopped (T >= 85C)
} thermal_state_t;

static volatile thermal_state_t g_thermal_state = THERM_STATE_NORMAL;
static volatile bool g_wdt_stopped = false;
static volatile bool g_clock_reduced = false;
static volatile u32 g_temp_readings = 0;
static volatile u32 g_clock_events = 0;
static volatile u32 g_wdt_events = 0;
static volatile i32 g_last_temp = 0;

/* Change the core clock the way the power manager does (ca_pwr_set_freq /
 * ca_pwr_restore_freq): the raw ca_os_clock_*_freq() alone left SysTick and
 * the UART baud tuned for the OLD clock, so the 75C branch would have halved
 * kernel time and turned the console to garbage at the very moment it reports
 * the overheat. ex59 tests the coordinated path; this is now that path. */
static void therm_set_clock(bool reduce)
{
    if (reduce) (void)ca_os_clock_reduce_freq(50);
    else        (void)ca_os_clock_restore_freq();
    (void)ca_os_tick_retune();
    ca_uart_retune();
    ca_temp_retune();       /* DTS alarm thresholds are counts of PCLK */
}


/* =============================================================================
 * TASK: Temperature Monitor
 * ============================================================================= */

/**
 * @brief Core temperature monitoring task
 * @details Reads the temperature every TEMP_READ_PERIOD_MS and activates protections:
 *          - 75C: reduces the core clock
 *          - 85C: stops WDT reload → system reset
 */
static u32 task_temp_monitor(void *arg) {
    (void)arg;

    i32 current_temp;
    thermal_state_t new_state;

    ca_stdout_printf_n("TEMP Monitor started");
    ca_stdout_printf_n("  - Clock reduce threshold: %dC", TEMP_CLOCK_THRESHOLD);
    ca_stdout_printf_n("  - WDT reset threshold: %dC", TEMP_WDT_RESET_THRESHOLD);
    ca_stdout_printf_n("  - Recovery threshold: %dC", TEMP_RECOVERY_THRESHOLD);
    ca_stdout_printf_n("  - Read period: %d ms", TEMP_READ_PERIOD_MS);

    while (1) {
        /* Read current temperature */
        current_temp = ca_temp_read();
        g_last_temp = current_temp;
        g_temp_readings++;

        /* Determine new state based on temperature */
        if (current_temp >= TEMP_WDT_RESET_THRESHOLD) {
            new_state = THERM_STATE_WDT_RESET;
        } else if (current_temp >= TEMP_CLOCK_THRESHOLD) {
            new_state = THERM_STATE_CLOCK_REDUCE;
        } else if (current_temp <= TEMP_RECOVERY_THRESHOLD) {
            new_state = THERM_STATE_NORMAL;
        } else {
            /* Maintain current state if in intermediate zone */
            new_state = g_thermal_state;
        }

        /* Handle state transition */
        if (new_state != g_thermal_state) {
            g_thermal_state = new_state;

            switch (g_thermal_state) {
                case THERM_STATE_WDT_RESET:
                    g_wdt_events++;
                    ca_stdout_printf_n("!!! WDT RESET: Temp=%dC - Stopping WDT reload!",
                                       current_temp);
                    g_wdt_stopped = true;
                    /* Reduce clock as well if not already reduced */
                    if (!g_clock_reduced) {
                        therm_set_clock(true);
                        g_clock_reduced = true;
                        ca_stdout_put("  -> Core clock: REDUCED (50%%)\r\n");
                    }
                    ca_stdout_put("  -> WDT reload: STOPPED (reset incoming!)\r\n");
                    break;

                case THERM_STATE_CLOCK_REDUCE:
                    if (!g_clock_reduced) {
                        g_clock_events++;
                        ca_stdout_printf_n("WARNING: Temp=%dC - Reducing clock",
                                           current_temp);
                        therm_set_clock(true);
                        g_clock_reduced = true;
                        ca_stdout_put("  -> Core clock: REDUCED (50%%)\r\n");
                    }
                    break;

                case THERM_STATE_NORMAL:
                    ca_stdout_printf_n("NORMAL: Temp=%dC - System OK",
                                       current_temp);
                    if (g_wdt_stopped) {
                        g_wdt_stopped = false;
                        ca_stdout_put("  -> WDT reload: RESTORED\r\n");
                    }
                    if (g_clock_reduced) {
                        therm_set_clock(false);
                        g_clock_reduced = false;
                        ca_stdout_put("  -> Core clock: RESTORED\r\n");
                    }
                    break;

                default:
                    break;
            }
        }

        /* Periodic print every 10 readings (5 seconds) */
        if (g_temp_readings % 10 == 0) {
            ca_stdout_printf_n("Temp=%dC, State=%d, Clk=%u, WDT=%u",
                               current_temp,
                               (u32)g_thermal_state,
                               g_clock_events,
                               g_wdt_events);
        }

        /* Wait for next reading cycle */
        ca_os_sleep(TEMP_READ_PERIOD_MS);
    }

    return 0;
}

/* =============================================================================
 * TASK: Watchdog Reload (integrated with thermal protection)
 * ============================================================================= */

/**
 * @brief Task that reloads the watchdog only if it has not been stopped due to overheating
 * @details When g_wdt_stopped is true, the WDT is not reloaded → system reset
 */
static u32 task_wdt_reload(void *arg) {
    (void)arg;

    ca_stdout_printf_n("WDT Reload task started");

    while (1) {
        /*
         * If the temperature has exceeded 85C, g_wdt_stopped is true
         * The WDT is NOT reloaded → reset after WDT_TIMEOUT_MS
         */
        if (!g_wdt_stopped) {
            ca_wdt_reload();
        }

        /*
         * Sleep for half of the watchdog timeout
         * Example: WDT_TIMEOUT_MS = 2000ms, sleep = 1000ms
         */
        ca_os_sleep(WDT_TIMEOUT_MS / 2);
    }

    return 0;
}

/* =============================================================================
 * TASK: System Monitor (optional - print statistics)
 * ============================================================================= */

/**
 * @brief System statistics monitoring task
 */
static u32 task_system_monitor(void *arg) {
    (void)arg;

    /* Wait for the system to stabilize */
    ca_os_sleep(3000);

    while (1) {
        ca_stdout_printf_n("=== SYSTEM STATUS ===");
        ca_stdout_printf_n("Temperature: %dC", g_last_temp);
        ca_stdout_printf_n("Thermal State: %d", (u32)g_thermal_state);
        ca_stdout_printf_n("Readings: %u", g_temp_readings);
        ca_stdout_printf_n("Clock Events: %u", g_clock_events);
        ca_stdout_printf_n("WDT Events: %u", g_wdt_events);
        ca_stdout_printf_n("WDT Stopped: %s", g_wdt_stopped ? "YES" : "NO");
        ca_stdout_printf_n("Clock Reduced: %s", g_clock_reduced ? "YES" : "NO");
        ca_stdout_printf_n("=====================");

        /* Print every 10 seconds */
        ca_os_sleep(10000);
    }

    return 0;
}


/* =============================================================================
 * DTS (H563): second sensor, independent of the ADC
 * ============================================================================= */

#if ENABLE_CLI
/* DS14258 table 102: DTS error -7..+2 C above 30 C; the ADC sensor adds its
 * 3 C of linearity (table 100). The two agree when DTS - ADC is in this window. */
#define DTS_DIFF_MIN    (-10)
#define DTS_DIFF_MAX    (5)

static volatile u32 s_dts_hot, s_dts_cold;

static void dts_alarm_cb(bool hot)          /* DTS interrupt */
{
    if (hot) s_dts_hot++; else s_dts_cold++;
}

static void dts_print_centi(const char *what, i32 c)
{
    const char sign = (c < 0) ? '-' : ' ';
    const u32  a    = (u32)((c < 0) ? -c : c);
    CLI_PRINTF_N("DTS %s %c%u.%02u C", what, sign, a / 100U, a % 100U);
}

static bool dts_compare(void)
{
    const u32 t0 = DWT->CYCCNT;
    const i32 d  = ca_temp_read_dts();
    const u32 us = (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000U);
    const i32 a  = ca_temp_read();
    dts_print_centi("temp ", d);
    CLI_PRINTF_N("DTS adc   %d C   read %u us", a, us);
    const i32 diff = d / 100 - a;
    const bool ok = (d != CA_TEMP_NO_DTS) && diff >= DTS_DIFF_MIN && diff <= DTS_DIFF_MAX;
    CLI_PRINTF_N("DTS CMP %s (DTS-ADC %d C, window %d..%d)", ok ? "PASS" : "FAIL",
                 diff, DTS_DIFF_MIN, DTS_DIFF_MAX);
    return ok;
}

/* Three steps: an alarm below the current temperature must fire (hot side
 * only), one around it must stay quiet, and must stay quiet across a halved
 * clock - the thresholds are PCLK counts, rebuilt by ca_temp_retune(). */
static void dts_alarm_test(void)
{
    const i32 t = ca_temp_read_dts() / 100;
    bool ok = true;

    s_dts_hot = s_dts_cold = 0U;
    (void)ca_temp_set_alarm(t - 30, t - 3, dts_alarm_cb);
    ca_os_sleep(500);
    const u32 h1 = s_dts_hot, c1 = s_dts_cold;
    CLI_PRINTF_N("DTS alarm %d..%d C at %d C: hot %u cold %u (want 1 0)", t - 30, t - 3, t, h1, c1);
    ok = ok && h1 == 1U && c1 == 0U;

    s_dts_hot = s_dts_cold = 0U;
    (void)ca_temp_set_alarm(t - 30, t + 30, dts_alarm_cb);
    ca_os_sleep(500);
    const u32 h2 = s_dts_hot, c2 = s_dts_cold;
    CLI_PRINTF_N("DTS alarm %d..%d C at %d C: hot %u cold %u (want 0 0)", t - 30, t + 30, t, h2, c2);
    ok = ok && h2 == 0U && c2 == 0U;

    therm_set_clock(true);
    ca_os_sleep(500);
    const u32 h3 = s_dts_hot, c3 = s_dts_cold;
    therm_set_clock(false);
    CLI_PRINTF_N("DTS alarm across half clock: hot %u cold %u (want 0 0)", h3, c3);
    ok = ok && h3 == 0U && c3 == 0U;

    (void)ca_temp_set_alarm(t - 30, t + 30, NULL);
    CLI_PRINTF_N("DTS ALARM %s", ok ? "PASS" : "FAIL");
}

static bool ex11_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("dts: [alarm] - digital sensor vs ADC, alarm test"); return false; }
    if (ca_strncmp(p, "dts", 3) != 0) return false;
    if (!ca_temp_dts_present()) {
        CLI_PUT_N("DTS: not present on this part");
        return true;
    }
    p = ca_skip_sp(p + 3);
    if (ca_strncmp(p, "alarm", 5) == 0) dts_alarm_test();
    else                                (void)dts_compare();
    return true;
}
static ca_cli_node_t s_ex11_cli = { ex11_cli, NULL };
#endif

/* =============================================================================
 * MAIN
 * ============================================================================= */

/* init hardware device enabled */
void ca_os_configure(){
    /* UART DBG or ITM ALREADY SET ON SYSTEM IF ENABLED! */
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(    
        ca_cli_init(4096);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_ex11_cli);
    )
    /* DBG */
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)
}


char buf[DEFAULT_MSG_SIZE];
void ca_os_main(void) {
    PRINTF_N(buf, sizeof(buf), "\r\n=================================================");
    PRINTF_N(buf, sizeof(buf), "  TEST 11: Core Temperature Monitor + WDT\r\n");
    PRINTF_N(buf, sizeof(buf), "=================================================\r\n\r\n");
EVAL_SENSOR(
    /* 1. Initialize the temperature sensor */
    PRINTF_N(buf, sizeof(buf), "Initializing temperature sensor...");
    if (!ca_temp_init()) {
        PRINTF_N(buf, sizeof(buf), "ERROR: Temperature sensor init FAILED!");
        ca_os_sleep(1000U);
        return;
    }
    PRINTF_N(buf, sizeof(buf), "Temperature sensor initialized");

    /* 2. Initialize the watchdog */
    PRINTF_N(buf, sizeof(buf), "Initializing WDT:");
    PRINTF_N(buf, sizeof(buf), "  - Timeout: %u ms", WDT_TIMEOUT_MS);
    PRINTF_N(buf, sizeof(buf), "  - Type: %s",
                     WDT_TYPE_USED == CA_WDT_TYPE_IWDG ? "IWDG" : "WWDG");
    PRINTF_N(buf, sizeof(buf), "  - Mode: %s",
                     WDT_MODE_USED == CA_WDT_MODE_RESET ? "RESET" : "INTERRUPT");

    if (!ca_wdt_init(WDT_TIMEOUT_MS, WDT_MODE_USED, WDT_TYPE_USED)) {
        PRINTF_N(buf, sizeof(buf), "ERROR: WDT initialization FAILED!");
        ca_os_sleep(1000U);
        return;
    }

    PRINTF_N(buf, sizeof(buf), "WDT initialized successfully");
    EVAL_CLI_WDT( PRINTF_N(buf, sizeof(buf), "CLI: type 'wdt info' to check WDT status"); )

    /* 3. Create tasks */

    /* Temperature monitoring task (high priority) */
    ca_os_job("temp_mon", CA_TH_LEVEL3, task_temp_monitor, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);

    /* Watchdog reload task (medium priority) */
    ca_os_job("wdt_reload", CA_TH_LEVEL2, task_wdt_reload, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    /* System monitoring task (low priority - optional) */
    ca_os_job("sys_mon", CA_TH_LEVEL1, task_system_monitor, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    
    /* 4. Enable the watchdog */
    PRINTF_N(buf, sizeof(buf), "Enabling WDT...");
    ca_os_delay_ms(100);  /* Wait for prints */

    if (!ca_wdt_enable()) {
        PRINTF_N(buf, sizeof(buf), "ERROR: WDT enable FAILED!");
        return;
    }

    PRINTF_N(buf, sizeof(buf), "WDT ENABLED - system protected");
    PRINTF_N(buf, sizeof(buf), "Temperature monitoring active");
    PRINTF_N(buf, sizeof(buf), "Clock reduce threshold: %dC", TEMP_CLOCK_THRESHOLD);
    PRINTF_N(buf, sizeof(buf), "WDT reset threshold: %dC", TEMP_WDT_RESET_THRESHOLD);
    PRINTF_N(buf, sizeof(buf), "Recovery threshold: %dC\r\n", TEMP_RECOVERY_THRESHOLD);
)
    /* 5. Start the scheduler */
    ca_os_run(true);
}
#else 
PRIVATE_STRING(m10a, "=== Example 11 ===");
PRIVATE_STRING(m10b, "EN_WDT / EN SENSOR  == 0, skipping temperature monitor + WDT example");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m10a);
    PUT_N(&priv_m10b);
}
void ca_os_main(void) { ca_os_run(true); }
#endif // ENABLE_SENSOR && EN_WDT

/* =============================================================================
 * IMPORTANT NOTES
 * =============================================================================
 *
 * 1. Temperature Sensor:
 *    - Uses internal MCU ADC
 *    - Typical accuracy: ±1C after calibration
 *    - Conversion time: ~10-20 μs
 *
 * 2. Two-Threshold Thermal Protection:
 *    - 75C (TEMP_CLOCK_THRESHOLD): reduces clock to 50%
 *    - 85C (TEMP_WDT_RESET_THRESHOLD): stops WDT reload → reset
 *    - 65C (TEMP_RECOVERY_THRESHOLD): restores normal configuration
 *
 * 3. Hysteresis:
 *    - Clock threshold: 75C (activation)
 *    - WDT reset threshold: 85C (activation)
 *    - Recovery threshold: 65C (deactivation of all protections)
 *    - 10-20C hysteresis prevents frequent oscillations
 *
 * 4. Watchdog Integration:
 *    - The wdt_reload task checks g_wdt_stopped before reloading
 *    - If true (T >= 85C), the WDT is not reloaded → reset after timeout
 *
 * 5. Clock Functions:
 *    - ca_os_clock_reduce_freq(50): reduces clock to 50%
 *    - ca_os_clock_restore_freq(): restores original clock
 *    - Implemented in hw/<family>/common/clock.c
 *
 * 6. Suggested Tests:
 *    a) Normal: temperature < 65C → all OK
 *    b) Clock reduce: 75C <= T < 85C → clock halved
 *    c) WDT reset: T >= 85C → system reset after ~2s
 *    d) Recovery: T drops below 65C → full restoration
 *
 * 7. Debug:
 *    - Use STM32CubeIDE to monitor global variables
 *    - g_thermal_state: current state (0=normal, 1=clock, 2=wdt)
 *    - g_clock_events: number of clock reduction activations
 *    - g_wdt_events: number of WDT reset activations
 *
 * =============================================================================
 */

