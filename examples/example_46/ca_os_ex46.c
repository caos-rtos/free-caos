// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 46 -- Secure pipe: encrypted ping-pong between two tasks
 *
 * Alice and Bob talk through a ca_secure_pipe_t (AES-GCM channel + one
 * queue per direction): encryption, transport and buffer wiping all
 * happen inside the service.
 *
 * Hardware: NUCLEO-H753ZI (STM32H753ZI)
 *
 * REQUIREMENTS os_config.h:
 *   EN_AES 1, EN_RNG 1, EN_KEYS_STORE 1, EN_AES_CHANNEL_SERVICE 1
 */

#include "caos.h"

#if EN_AES && EN_RNG && EN_KEYS_STORE && EN_AES_CHANNEL_SERVICE


#define PING_INTERVAL_MS  1000U
#define PING_COUNT        100U

static const u8 s_test_key[32] = {
    0x60,0x3d,0xeb,0x10, 0x15,0xca,0x71,0xbe,
    0x2b,0x73,0xae,0xf0, 0x85,0x7d,0x77,0x81,
    0x1f,0x35,0x2c,0x07, 0x3b,0x61,0x08,0xd7,
    0x2d,0x98,0x10,0xa3, 0x09,0x14,0xdf,0xf4,
};

#define PIPE_DEPTH  4U

static ca_secure_pipe_t s_pipe;
static void            *s_key_ptr;

static u32 task_hb(void *arg)
{
    (void)arg;
    while (1) { ca_led_toggle(1U); ca_os_sleep(500); }
    return 0;
}

static u32 task_alice(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("=== EX46: AES-GCM ping-pong (secure pipe) ===");

    u32 ok_count = 0;
    for (u32 ping = 0; ping < PING_COUNT; ping++) {
        char plain[64];
        u32 plen = (u32)ca_snprintf(plain, sizeof(plain),
                                    "PING #%u from Alice", (unsigned)ping);
        i32 rc = ca_secure_pipe_send(&s_pipe, CA_SECURE_PIPE_A, plain, plen, 2000);
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_printf_n("Alice: %s #%u",
                               rc == CA_SECURE_PIPE_TIMEOUT ? "send FAIL" : "enc FAIL",
                               (unsigned)ping);
            ca_os_sleep(PING_INTERVAL_MS); continue;
        }
        ca_stdout_printf_n("Alice TX: '%s'", plain);

        char dec[64];
        u32  dec_len = 0;
        rc = ca_secure_pipe_recv(&s_pipe, CA_SECURE_PIPE_A, dec, sizeof(dec) - 1U,
                                 &dec_len, 5000);
        if (rc == CA_SECURE_PIPE_TIMEOUT) {
            ca_stdout_put_n("Alice: echo timeout"); continue;
        }
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_put_n("Alice: dec FAIL (tag mismatch?)"); continue;
        }
        dec[dec_len] = '\0';

        char expected[64];
        u32 exp_len = (u32)ca_snprintf(expected, sizeof(expected),
                                       "PONG #%u from Bob", (unsigned)ping);
        bool match = (dec_len == exp_len) && (ca_memcmp(dec, expected, dec_len) == 0);
        ca_stdout_printf_n("Alice RX: '%s' - %s", dec, match ? "OK" : "MISMATCH");
        if (match) ok_count++;
        ca_os_sleep(PING_INTERVAL_MS);
    }

    ca_stdout_printf_n("Alice: %u/%u ping OK", (unsigned)ok_count, (unsigned)PING_COUNT);
    ca_stdout_put_n("Alice: test done.");
    return 0;
}

static u32 task_bob(void *arg)
{
    (void)arg;
    while (1) {
        char dec[64];
        u32  dec_len = 0;
        i32 rc = ca_secure_pipe_recv(&s_pipe, CA_SECURE_PIPE_B, dec, sizeof(dec) - 1U,
                                     &dec_len, 10000);
        if (rc == CA_SECURE_PIPE_TIMEOUT) {
            ca_stdout_put_n("Bob: timeout"); continue;
        }
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_put_n("Bob: dec FAIL"); continue;
        }
        dec[dec_len] = '\0';
        ca_stdout_printf_n("Bob RX: '%s'", dec);

        u32 ping_num = 0;
        for (u32 i = 0; i < dec_len; i++) {
            if (dec[i] == '#') {
                for (u32 j = i+1; j < dec_len && dec[j] >= '0' && dec[j] <= '9'; j++)
                    ping_num = ping_num * 10 + (u32)(dec[j] - '0');
                break;
            }
        }

        char pong[64];
        u32 pong_len = (u32)ca_snprintf(pong, sizeof(pong),
                                        "PONG #%u from Bob", (unsigned)ping_num);
        rc = ca_secure_pipe_send(&s_pipe, CA_SECURE_PIPE_B, pong, pong_len, 2000);
        if (rc != CA_SECURE_PIPE_OK) {
            ca_stdout_put_n(rc == CA_SECURE_PIPE_TIMEOUT ? "Bob: reply FAIL" : "Bob: enc FAIL");
            continue;
        }
        ca_stdout_printf_n("Bob TX: '%s'", pong);
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
        ca_cli_init(1024);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
    ca_led_init();
    ca_led_off(1U);

    if (!ca_aes_init()) CA_SAFE_FAIL("ca_aes_init");
    if (!ca_rng_init())  CA_SAFE_FAIL("ca_rng_init");

    if (ca_keys_store_init(4) != 0) CA_SAFE_FAIL("ca_keys_store_init");
    s_key_ptr = ca_keys_store_allocate(32);
    if (!s_key_ptr) CA_SAFE_FAIL("ca_keys_store_allocate");
    ca_keys_store_temp_enable_access(s_key_ptr, CA_KEYS_STORE_READ_WRITE);
    ca_keys_store_copy(s_key_ptr, s_test_key, 32);
    ca_keys_store_lock_completely(s_key_ptr);

    if (ca_secure_pipe_init(&s_pipe, CA_AES_MODE_GCM, CA_AES_KEY_256,
                            s_key_ptr, PIPE_DEPTH) != CA_SECURE_PIPE_OK)
        CA_SAFE_FAIL("ca_secure_pipe_init");
}

void ca_os_main(void)
{
    ca_os_job("alice", CA_TH_LEVEL4, task_alice, NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("bob",   CA_TH_LEVEL4, task_bob,   NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("hb",    CA_TH_LEVEL3, task_hb,    NULL, STACK_SMALL,  CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else  /* EN_AES && EN_RNG && EN_KEYS_STORE && EN_AES_CHANNEL_SERVICE not all enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m46a, "=== Example 46 ===");
PRIVATE_STRING(m46b, "EN_AES / EN_RNG / EN_KEYS_STORE / EN_AES_CHANNEL_SERVICE disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m46a);
    PUT_N(&priv_m46b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
