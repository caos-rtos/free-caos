// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex66.c
 * @brief          : Example 66 — crypto self-test (SHA-512, X25519, Ed25519, P-256)
 ******************************************************************************
 * @details
 * Validation of the cryptographic primitives that form the basis of the
 * SSH server stack, each against the official vectors of its own
 * standard. None of these pass through hardware: SHA-512 is not
 * supported by the HASH IP (which only covers SHA-1/SHA-256) and the
 * Edwards/Montgomery curves have no accelerator on these families.
 *
 *   SHA-512   NIST FIPS 180-4    6 tests   prerequisite for Ed25519
 *   X25519    RFC 7748           8 tests   key exchange curve25519-sha256
 *   Ed25519   RFC 8032           8 tests   host key + user auth
 *   P-256     RFC 6979 A.2.5    10 tests   ECDSA for the TLS 1.3 server
 *
 * At startup, the task executes the three batteries in sequence and prints the
 * overall result with the time taken. The following remain available from the console:
 *
 *   sha512 test        re-runs the NIST vectors
 *   x25519 test        re-runs the RFC 7748 vectors
 *   ed25519 test       re-runs the RFC 8032 vectors
 *   ed25519 keygen     generates a key pair from the hardware RNG
 *   p256 test          re-runs the RFC 6979 vectors (prints sign/verify ms)
 *   p256 keygen        generates a P-256 key pair from the RNG
 *
 * The verification is not a formality: an incorrect cryptographic primitive
 * still produces random-looking output, and the error would only emerge
 * much further downstream as an handshake that fails without explanation.
 *
 * No specific hardware required (pure calculation), except for
 * "ed25519 keygen" which uses the RNG: on F1xx, lacking the peripheral,
 * generation fails cleanly instead of producing weak keys.
 ******************************************************************************
 */

#include "caos.h"

#if EN_SHA512_TEST && EN_X25519_TEST && EN_ED25519_TEST

/* ============================================================================
 * Task
 * ========================================================================== */

/* Each battery runs in its OWN task, so a stack overflow in one takes only
 * that one down: the others still report, and the summary names the ones that
 * never finished.
 *
 * They used to share a task, which made a failure unreadable: on the F446
 * X25519 blew the guard band, the task was removed, and SHA-512's result went
 * with it while Ed25519 never started. One task each also lets every battery
 * ask for the stack depth it actually needs (the `need` field below).
 *
 * Created ONE AT A TIME by the orchestrator, each after the previous has
 * finished, so only one battery holds a stack at any moment and their output
 * cannot interleave. An earlier version created all four up front with
 * ca_os_job_suspend: that serialised the output just as well, but four
 * suspended tasks still reserve their stacks, and 16+16+32+32 KB of batteries
 * did not fit alongside stdout, cli and the orchestrator in the 116 KB PSP
 * area of the U575 - the fourth was simply never created. */

typedef enum {
    T_PENDING = 0,   /* created, has not reported back - died if it stays here */
    T_PASS,
    T_FAIL
} test_state_t;

typedef struct {
    const char   *name;
    u32         (*run)(void);
    ca_stack_size_t  need;    /* how deep this battery goes - see below */
    test_state_t  state;
    u32           fails;
    u32           ms;
    u32           stack;   /* stable handle: a thid would go stale, see below */
} crypto_test_t;

/* The stack each battery asks for is part of the test data, because it is not
 * the same for all of them. Hashing is flat: SHA-256 and SHA-512 pass 6/6
 * inside STACK_LARGE on every part. Curve arithmetic is not: X25519 nests
 * field operations over 32-byte scalars and overflowed a 4064-byte guard band
 * by 136 bytes on the F446, taking the task with it, and Ed25519 did the same
 * from the CLI's own stack. Both pass on the U575, where STACK_LARGE happens
 * to be 16 KB against 4 KB here.
 *
 * So the two curves get STACK_KERNEL instead of raising STACK_MIN for the
 * whole system: the depth is paid by the tasks that need it, not by every task
 * on a part with 128 KB of RAM and no CCMRAM. */
/* Guarded on BOTH flags, as os_config's own EVAL_*_TEST macros are: a table of
 * function pointers lives in .data, so the linker demands every symbol named
 * here whether or not the code paths using it survive. Testing only the _TEST
 * half left ca_x25519_selftest undefined on a part where EN_X25519 is off. */
static crypto_test_t s_tests[] = {
#if EN_SHA256_TEST && EN_SHA256
    { "SHA-256", ca_sha256_selftest,  STACK_LARGE,  T_PENDING, 0U, 0U, 0U },
#endif
#if EN_SHA512_TEST && EN_SHA512
    { "SHA-512", ca_sha512_selftest,  STACK_LARGE,  T_PENDING, 0U, 0U, 0U },
#endif
#if EN_X25519_TEST && EN_X25519
    { "X25519",  ca_x25519_selftest,  STACK_KERNEL, T_PENDING, 0U, 0U, 0U },
#endif
#if EN_ED25519_TEST && EN_ED25519
    { "Ed25519", ca_ed25519_selftest, STACK_KERNEL, T_PENDING, 0U, 0U, 0U },
#endif
#if EN_ECDSA_P256_TEST && EN_ECDSA_P256
    { "P-256",   ca_ecdsa_p256_selftest, STACK_KERNEL, T_PENDING, 0U, 0U, 0U },
#endif
};
#define N_TESTS  (sizeof(s_tests) / sizeof(s_tests[0]))

/* One battery. The kernel calls a task as f(idx, p1): the FIRST argument is
 * the task's own slot in the task list, and the value handed to ca_os_job is
 * the SECOND. Reading the index out of the first one indexed s_tests[] with a
 * slot number - 4, 5 - past the end of a four-entry array, which is how this
 * ended up calling a garbage pointer at 0x0A037A00. */
static u32 task_one_test(void *arg)
{
    const u32 p1 = (u32)(uintptr_t)arg;
    crypto_test_t *t = &s_tests[p1];

    u32 t0 = ca_os_tick_ms();
    /* Stack window and free CCM printed before and after the battery: on the
     * U575 Ed25519 dies with STACK_KERNEL = 32 KB while it passes on the F446
     * with 8 KB, so the number that matters is not how much was asked for but
     * where the block actually landed and how much of it was consumed. */
    EVAL_STDOUT(
        /* t->need is the size asked for; t->stack is the base the kernel
         * handed back - printing the latter as "req" was simply wrong. */
        ca_stdout_printf_n("ex66: running %s... (need=%u B, base=0x%08X, psp=0x%08X)",
                           t->name, (unsigned)t->need, (unsigned)t->stack,
                           (unsigned)__get_PSP());
    )
    u32 f  = t->run();
    t->ms  = ca_os_tick_ms() - t0;

    EVAL_STDOUT(
        ca_stdout_printf_n("ex66: %s returned (psp=0x%08X)",
                           t->name, (unsigned)__get_PSP());
    )

    /* Reached only if the battery returned: an overflow removes the task
     * inside run(), and the entry stays T_PENDING - which is the report. */
    t->fails = f;
    t->state = (f == 0U) ? T_PASS : T_FAIL;
    return f;
}

/* Releases the batteries one at a time, then prints the summary. */
static u32 task_selftest(void *arg)
{
    (void)arg;

    u32 i;

    /* Allow time for the console to activate before printing */
    ca_os_sleep(2000);
     EVAL_STDOUT(ca_stdout_printf_n("\r\n=== Example 66: crypto self-test ===");)
     EVAL_STDOUT(ca_stdout_printf_n("SHA-512 (FIPS 180-4), X25519 (RFC 7748), Ed25519 (RFC 8032), P-256 (RFC 6979)");)
     EVAL_STDOUT(ca_stdout_printf_n("SSH server stack base primitives");)
     EVAL_STDOUT(ca_os_sleep(100);)
    

    /* One battery is created, run and reaped before the next is created.
     *
     * They used to be created all together, suspended, before ca_os_run().
     * Four suspended tasks hold their stacks at the same time even though only
     * one runs, and on the U575 that does not fit: 16+16+32+32 KB of batteries
     * plus 8 each for stdout, cli and this orchestrator is 120 KB against the
     * 116 KB of PSP area (_psp_stack_base..top). The FOURTH one - Ed25519 -
     * got NULL from ccm_alloc_stack and was never created, which the summary
     * then reported as "DID NOT FINISH (task gone: stack?)" and the banner as
     * "needs a deeper stack on this part". Both misleading: Ed25519 does not
     * need a deeper stack, it needs the other three not to be holding theirs.
     * Creating them in turn keeps one big block alive at a time - peak 56 KB.
     *
     * A battery that exits returns its stack to the free-list, so the next
     * one reuses the very same block when the size matches. */
    for (i = 0U; i < N_TESTS; i++) {
        EVAL_STDOUT( ca_stdout_put_n(""); )

        const ca_task_handle_t h = ca_os_job(s_tests[i].name, CA_TH_LEVEL3,
                                             task_one_test, (void *)(uintptr_t)i,
                                             s_tests[i].need, CA_JOB_STD, CA_FAULT_ASSERT);
        if (h == CA_TASK_HANDLE_NONE) {
            /* Say what actually happened instead of leaving it to look like an
             * overflow: no task slot, or no room left in the PSP area. */
            EVAL_STDOUT( ca_stdout_printf_n(
                "ex66:   %-8s NOT CREATED - no free task slot or no PSP left",
                s_tests[i].name); )
            continue;
        }
        s_tests[i].stack = ca_os_stack_from_handle(h);

        /* No join in the kernel, so the state field is the handshake: a
         * battery leaves T_PENDING on its way out. One killed by the stack
         * guard never does, hence the deadline - without it this would wait
         * forever on exactly the case this example exists to find. */
        u32 waited = 0U;
        while (s_tests[i].state == T_PENDING && waited < 20000U) {
            ca_os_sleep(50);
            waited += 50U;
        }

        /* Let the kernel finish removing the task before creating the next:
         * the stack goes back to the free-list during that sweep, and the
         * next battery is meant to reuse it. */
        ca_os_sleep(100);
    }

    u32 fails = 0U, dead = 0U, total_ms = 0U;

    EVAL_STDOUT( ca_stdout_put_n(""); )
    EVAL_STDOUT( ca_stdout_put_n("ex66: --- results ---"); )
    for (i = 0U; i < N_TESTS; i++) {
        crypto_test_t *t = &s_tests[i];
        total_ms += t->ms;
        if (t->state == T_PENDING) {
            dead++;
            EVAL_STDOUT( ca_stdout_printf_n("ex66:   %-8s DID NOT FINISH (task gone: stack?)",
                                            t->name); )
        } else if (t->state == T_FAIL) {
            fails += t->fails;
            EVAL_STDOUT( ca_stdout_printf_n("ex66:   %-8s %u FAILED  (%u ms)",
                                            t->name, t->fails, t->ms); )
        } else {
            EVAL_STDOUT( ca_stdout_printf_n("ex66:   %-8s ok  (%u ms)", t->name, t->ms); )
        }
    }

    EVAL_STDOUT( ca_stdout_printf_n("ex66: total elapsed %u ms", total_ms); )

    if (fails == 0U && dead == 0U) {
        EVAL_STDOUT( ca_stdout_put_n("ex66: primitives validated - ready for SSH"); )
    } else if (dead != 0U) {
        /* "needs a deeper stack" is only ONE of the two reasons, and it used
         * to be the wrong one: a battery that never got created for lack of
         * PSP room lands here too, and says so at creation time. */
        EVAL_STDOUT( ca_stdout_printf_n(
            "ex66: %u batter%s never finished - overflowed, or never created",
            dead, (dead == 1U) ? "y" : "ies"); )
    } else {
        EVAL_STDOUT( ca_stdout_printf_n("ex66: %u TESTS FAILED - do not proceed", fails); )
    }

    EVAL_STDOUT( ca_stdout_put_n("ex66: 'sha512|x25519|ed25519|p256 test' to run again"); )
    return fails + dead;
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
    /* keygen (ed25519/p256) draws from the RNG: never initialised here before,
     * so both answered "RNG not available". A failure stays non-fatal: the
     * vectors need no entropy, only keygen is lost. */
    EVAL_RNG( (void)ca_rng_init(); )

    /* CLI command registration */
    EVAL_SHA256_TEST(  ca_sha256_test_init();  )
    EVAL_SHA512_TEST(  ca_sha512_test_init();  )
    EVAL_X25519_TEST(  ca_x25519_test_init();  )
    EVAL_ED25519_TEST( ca_ed25519_test_init(); )
    EVAL_ECDSA_P256_TEST( ca_ecdsa_p256_test_init(); )

    /* The batteries are NOT created here any more: task_selftest creates each
     * one when its turn comes, so only one of them holds a stack at a time.
     * Created all together and suspended they did not fit in the PSP area on
     * the U575 - see the comment in the loop.
     *
     * They keep CA_FAULT_ASSERT (0x00: no restart bit) rather than
     * RESTART_THREAD - a task restarted after a stack overflow would overflow
     * again, forever. */
    ca_os_job("cryptot", CA_TH_LEVEL3, task_selftest, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_run(true);
}

#else  /* no self-test enabled */

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m66a, "=== Example 66 ===");
PRIVATE_STRING(m66b, "EN_SHA512_TEST / EN_X25519_TEST / EN_ED25519_TEST disabled");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m66a);
    PUT_N(&priv_m66b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif /* self-tests enabled */
