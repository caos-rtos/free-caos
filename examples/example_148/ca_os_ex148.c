// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex148.c
 * @brief          : Example 148 — MicroPython and the caOS services
 ******************************************************************************
 * @details
 * The REPL of example 145 with the caOS services a script can reach:
 *  - standard modules on caOS: os.urandom (RNG), time.time / localtime (RTC);
 *  - the caos module: system information, LEDs, crypto (hashes, HMAC, KDF,
 *    AES-GCM, X25519, Ed25519, ECDSA P-256, CRC32) and the keystore, used by
 *    index only: a key value never reaches Python.
 *
 * A C task, py_peer, talks to Python through the shared-object registry: it
 * publishes the queues 'py2c' and 'c2py' (8-byte items) and the event group
 * 'cev'; each item it receives on py2c comes back reversed on c2py, with bit 0
 * of cev set. When a script creates a queue named 'pyq', py_peer finds it
 * (ca_python_find_queue) and sends one item, b'from-c!!', into it.
 *
 *   py             MicroPython REPL (Ctrl-D on an empty line exits)
 ******************************************************************************
 */

#include "caos.h"

#if EN_PYTHON && ENABLE_CLI

#define PEER_ITEM  8U

static ca_queue_t       s_py2c, s_c2py;
#if EN_EVENT
static ca_event_group_t s_cev;
#endif

static u32 py_peer(void *arg)
{
    (void)arg;
    (void)ca_python_publish_queue("py2c", &s_py2c, PEER_ITEM);
    (void)ca_python_publish_queue("c2py", &s_c2py, PEER_ITEM);
#if EN_EVENT
    (void)ca_python_publish_event("cev", &s_cev);
#endif
    bool told_pyq = false;
    for (;;) {
        u8 in[PEER_ITEM], out[PEER_ITEM];
        if (ca_queue_heap_receive(&s_py2c, in, 50U)) {
            for (u32 i = 0U; i < PEER_ITEM; i++) out[i] = in[PEER_ITEM - 1U - i];
            (void)ca_queue_heap_send(&s_c2py, out, 100U);
#if EN_EVENT
            (void)ca_event_set(&s_cev, 0x1U);
#endif
        }
        /* a queue made by Python, found from C by its name */
        u32 isz = 0U;
        ca_queue_t *q = ca_python_find_queue("pyq", &isz);
        if (q == NULL) {
            told_pyq = false;               /* gone with the VM: tell the next one */
        } else if (!told_pyq && isz == PEER_ITEM) {
            told_pyq = ca_queue_heap_send(q, "from-c!!", 0U);
        }
    }
    return 0;
}

void ca_os_configure(void)
{
    EVAL_CRC( ca_crc32_init(); )
    EVAL_AES( ca_aes_init(); )
    EVAL_RNG( ca_rng_init(); )
    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
    )
#if EN_KEYS_STORE
    (void)ca_keys_store_init(8);
#endif
#if EN_LED
    ca_led_init();
#endif
}

void ca_os_main(void)
{
    (void)ca_queue_heap_init(&s_py2c, PEER_ITEM, 4U);
    (void)ca_queue_heap_init(&s_c2py, PEER_ITEM, 4U);
#if EN_EVENT
    ca_event_init(&s_cev);
#endif
    ca_os_job("py_peer", CA_TH_LEVEL3, py_peer, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else

PRIVATE_STRING(m148a, "=== Example 148 ===");
PRIVATE_STRING(m148b, "ex148: EN_PYTHON or ENABLE_CLI disabled");
void ca_os_configure(void)
{
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m148a);
    PUT_N(&priv_m148b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
