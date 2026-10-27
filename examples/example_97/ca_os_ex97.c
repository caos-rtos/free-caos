// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 97 -- Secure pipe LINK mode: the encrypted board-to-board channel,
 * both ends on one board
 *
 * The link of secure_pipe_service seals and opens buffers; the application
 * carries them (UDP in workspace/app_3). Here the two ends - role 1 and
 * role 2 - live on the same board and hand the buffers to each other
 * directly, so every security property can be checked before the link is
 * put on a cable.
 *
 * CLI:
 *   link test    run all the phases, one PASS/FAIL line each, then
 *                "LINK RESULT PASS n/n" or "LINK RESULT FAIL n/n"
 *
 * The phases:
 *   init           PBKDF2 (100000 iterations) + HKDF on both ends, timed
 *   not_up         seal before any handshake is refused
 *   bad_password   a HELLO from a different password is rejected
 *   handshake      role 1 starts, role 2 replies, both derive the session;
 *                  the reply seen again is ignored (no reply loop)
 *   roundtrip      1->2 and 2->1, 1/16/100/1400 bytes, with AAD
 *   tamper_body    one ciphertext bit flipped -> rejected, output wiped
 *   tamper_len     length field lowered -> rejected (no silent truncation)
 *   tamper_aad     different AAD (e.g. another UDP flow) -> rejected
 *   replay         the same message twice -> second rejected
 *   reorder        b before a accepted (window), a again rejected
 *   too_old        older than the 64-message window -> rejected
 *   reflection     an end opening its own message -> rejected (K12 != K21)
 *   peer_restart   role 2 restarts: new handshake, the old session's
 *                  message no longer opens, the new session works
 *
 * Hardware: any board with AES (CRYP or the software path) and RNG.
 *
 * REQUIREMENTS os_config.h:
 *   EN_AES 1, EN_RNG 1, EN_KEYS_STORE 1, EN_AES_CHANNEL_SERVICE 1, EN_SHA256 1
 */

#include "caos.h"

#if EN_AES && EN_RNG && EN_KEYS_STORE && EN_AES_CHANNEL_SERVICE && EN_SHA256

#define PASSWORD      "caos-banco-97"
#define PASSWORD_BAD  "caos-banco-98"
#define LINK_NAME     "link-ex97"
#define BUF_MAX       1500U

static ca_sp_link_t   s_l1, s_l2, s_bad;
static ca_semaphore_t s_go;
static volatile bool  s_running;

/* static: too large for the task stack */
static u8 s_plain[BUF_MAX];
static u8 s_wire[BUF_MAX + CA_SP_LINK_OVERHEAD_MAX];
static u8 s_wire2[BUF_MAX + CA_SP_LINK_OVERHEAD_MAX];
static u8 s_out[BUF_MAX + 16U];

/* app_3 will pass IP addresses and UDP ports here */
static const u8 AAD_FLOW_A[12] = { 192,168,10,1, 192,168,20,1, 0x13,0x88, 0x13,0x88 };
static const u8 AAD_FLOW_B[12] = { 192,168,10,1, 192,168,20,2, 0x13,0x88, 0x13,0x88 };

static u32 s_pass, s_total;

static void phase(const char *name, bool ok, const char *detail)
{
    s_total++;
    if (ok) s_pass++;
    ca_stdout_printf_n("[link] %-13s %s  %s", name, ok ? "PASS" : "FAIL", detail);
}

static void fill(u32 len, u8 seed)
{
    for (u32 i = 0U; i < len; i++) s_plain[i] = (u8)(seed + i * 7U);
}

/* seal at `from`, open at `to`, compare */
static bool roundtrip(ca_sp_link_t *from, ca_sp_link_t *to, u32 len, u8 seed)
{
    u32 wl = 0U, ol = 0U;
    fill(len, seed);
    if (ca_sp_link_seal(from, AAD_FLOW_A, sizeof(AAD_FLOW_A), s_plain, len,
                        s_wire, sizeof(s_wire), &wl) != CA_SECURE_PIPE_OK) return false;
    if (ca_sp_link_open(to, AAD_FLOW_A, sizeof(AAD_FLOW_A), s_wire, wl,
                        s_out, sizeof(s_out), &ol) != CA_SECURE_PIPE_OK) return false;
    return ol == len && ca_memcmp(s_out, s_plain, len) == 0;
}

static i32 seal(ca_sp_link_t *l, u8 *wire, u32 *wl, u32 len, u8 seed)
{
    fill(len, seed);
    return ca_sp_link_seal(l, AAD_FLOW_A, sizeof(AAD_FLOW_A), s_plain, len,
                           wire, sizeof(s_wire), wl);
}

static i32 open_(ca_sp_link_t *l, const u8 *wire, u32 wl)
{
    u32 ol = 0U;
    return ca_sp_link_open(l, AAD_FLOW_A, sizeof(AAD_FLOW_A), wire, wl,
                           s_out, sizeof(s_out), &ol);
}

/* 1 says HELLO, 2 answers, both up */
static bool handshake(ca_sp_link_t *a, ca_sp_link_t *b)
{
    u8  h[CA_SP_LINK_HELLO_SIZE];
    u32 hl = 0U;
    if (ca_sp_link_hello(a, false, h, sizeof(h), &hl) != CA_SECURE_PIPE_OK) return false;
    if (ca_sp_link_accept(b, h, hl) != CA_SECURE_PIPE_REPLY) return false;
    if (ca_sp_link_hello(b, true, h, sizeof(h), &hl) != CA_SECURE_PIPE_OK) return false;
    if (ca_sp_link_accept(a, h, hl) != CA_SECURE_PIPE_OK) return false;
    /* the reply again: late or replayed, must not start a new exchange */
    if (ca_sp_link_accept(a, h, hl) != CA_SECURE_PIPE_REPLAY) return false;
    return ca_sp_link_is_up(a) && ca_sp_link_is_up(b);
}

/* before deinit: it wipes the whole struct, counters included */
static void print_stats(const char *who, const ca_sp_link_t *l)
{
    ca_stdout_printf_n("%s hello ok=%u bad=%u sessions=%u", who, (unsigned)l->st.hello_ok,
                       (unsigned)l->st.hello_bad, (unsigned)l->st.sessions);
    ca_stdout_printf_n("%s sealed=%u opened=%u tag_fail=%u replay=%u bad_format=%u", who,
                       (unsigned)l->st.sealed, (unsigned)l->st.opened, (unsigned)l->st.tag_fail,
                       (unsigned)l->st.replay, (unsigned)l->st.bad_format);
}

static void run_tests(void)
{
    char d[96];
    s_pass = s_total = 0U;
    ca_stdout_put_n("=== EX97: secure pipe link, both ends on this board ===");

    /* init */
    u32 t0 = ca_os_tick_ms();
    bool ok = ca_sp_link_init(&s_l1, 1U, PASSWORD, LINK_NAME) == CA_SECURE_PIPE_OK &&
              ca_sp_link_init(&s_l2, 2U, PASSWORD, LINK_NAME) == CA_SECURE_PIPE_OK;
    ca_snprintf(d, sizeof(d), "two ends derived in %u ms", (unsigned)(ca_os_tick_ms() - t0));
    phase("init", ok, d);
    if (!ok) goto end;

    /* not_up + bad_password, on an end with the wrong password */
    {
        u32 wl = 0U;
        bool bad_ok = ca_sp_link_init(&s_bad, 2U, PASSWORD_BAD, LINK_NAME) == CA_SECURE_PIPE_OK;
        i32 rc = seal(&s_bad, s_wire, &wl, 16U, 1U);
        phase("not_up", bad_ok && rc == CA_SECURE_PIPE_NOT_UP, "seal before handshake refused");

        u8  h[CA_SP_LINK_HELLO_SIZE];
        u32 hl = 0U;
        (void)ca_sp_link_hello(&s_l1, false, h, sizeof(h), &hl);
        rc = ca_sp_link_accept(&s_bad, h, hl);
        ca_snprintf(d, sizeof(d), "accept rc=%d, hello_bad=%u", (int)rc,
                    (unsigned)s_bad.st.hello_bad);
        phase("bad_password", rc == CA_SECURE_PIPE_DECRYPT && !ca_sp_link_is_up(&s_bad), d);
        ca_sp_link_deinit(&s_bad);
    }

    /* handshake */
    ok = handshake(&s_l1, &s_l2);
    phase("handshake", ok, ok ? "both ends up" : "session not derived");
    if (!ok) goto end;

    /* roundtrip */
    {
        static const u32 sizes[] = { 1U, 16U, 100U, 1400U };
        u32 good = 0U;
        for (u32 i = 0U; i < 4U; i++) {
            if (roundtrip(&s_l1, &s_l2, sizes[i], (u8)i)) good++;
            if (roundtrip(&s_l2, &s_l1, sizes[i], (u8)(i + 9U))) good++;
        }
        ca_snprintf(d, sizeof(d), "%u/8 (1,16,100,1400 B, both ways)", (unsigned)good);
        phase("roundtrip", good == 8U, d);
    }

    /* tamper_body: one bit of ciphertext */
    {
        u32 wl = 0U;
        (void)seal(&s_l1, s_wire, &wl, 100U, 3U);
        s_wire[CA_SP_LINK_DATA_HDR + 5U] ^= 0x01U;
        ca_memset(s_out, 0xA5U, 100U);
        i32 rc = open_(&s_l2, s_wire, wl);
        bool wiped = true;
        for (u32 i = 0U; i < 100U; i++) if (s_out[i] != 0U) { wiped = false; break; }
        ca_snprintf(d, sizeof(d), "rc=%d, output %s", (int)rc, wiped ? "wiped" : "NOT wiped");
        phase("tamper_body", rc == CA_SECURE_PIPE_DECRYPT && wiped, d);
    }

    /* tamper_len: 100 -> 97, still inside the padding: only the tag can tell */
    {
        u32 wl = 0U;
        (void)seal(&s_l1, s_wire, &wl, 100U, 4U);
        s_wire[10] = 97U;
        i32 rc = open_(&s_l2, s_wire, wl);
        ca_snprintf(d, sizeof(d), "rc=%d", (int)rc);
        phase("tamper_len", rc == CA_SECURE_PIPE_DECRYPT, d);
    }

    /* tamper_aad: moved to another flow */
    {
        u32 wl = 0U, ol = 0U;
        (void)seal(&s_l1, s_wire, &wl, 64U, 5U);
        i32 rc = ca_sp_link_open(&s_l2, AAD_FLOW_B, sizeof(AAD_FLOW_B), s_wire, wl,
                                 s_out, sizeof(s_out), &ol);
        ca_snprintf(d, sizeof(d), "rc=%d", (int)rc);
        phase("tamper_aad", rc == CA_SECURE_PIPE_DECRYPT, d);
    }

    /* replay */
    {
        u32 wl = 0U;
        (void)seal(&s_l1, s_wire, &wl, 32U, 6U);
        i32 r1 = open_(&s_l2, s_wire, wl);
        i32 r2 = open_(&s_l2, s_wire, wl);
        ca_snprintf(d, sizeof(d), "first rc=%d, again rc=%d", (int)r1, (int)r2);
        phase("replay", r1 == CA_SECURE_PIPE_OK && r2 == CA_SECURE_PIPE_REPLAY, d);
    }

    /* reorder: b before a */
    {
        u32 la = 0U, lb = 0U;
        (void)seal(&s_l1, s_wire, &la, 40U, 7U);
        (void)seal(&s_l1, s_wire2, &lb, 40U, 8U);
        i32 rb = open_(&s_l2, s_wire2, lb);
        i32 ra = open_(&s_l2, s_wire, la);
        i32 ra2 = open_(&s_l2, s_wire, la);
        ca_snprintf(d, sizeof(d), "b=%d a=%d a-again=%d", (int)rb, (int)ra, (int)ra2);
        phase("reorder", rb == CA_SECURE_PIPE_OK && ra == CA_SECURE_PIPE_OK &&
                         ra2 == CA_SECURE_PIPE_REPLAY, d);
    }

    /* too_old: 70 messages, the newest opened first */
    {
        u32 lold = 0U, wl = 0U;
        (void)seal(&s_l1, s_wire2, &lold, 20U, 10U);           /* oldest */
        for (u32 i = 0U; i < 69U; i++) (void)seal(&s_l1, s_wire, &wl, 20U, 11U);
        i32 rn = open_(&s_l2, s_wire, wl);                     /* newest */
        i32 ro = open_(&s_l2, s_wire2, lold);
        ca_snprintf(d, sizeof(d), "newest=%d, 69 behind=%d", (int)rn, (int)ro);
        phase("too_old", rn == CA_SECURE_PIPE_OK && ro == CA_SECURE_PIPE_REPLAY, d);
    }

    /* reflection: an end opening its own message */
    {
        u32 wl = 0U;
        (void)seal(&s_l1, s_wire, &wl, 48U, 12U);
        i32 rc = open_(&s_l1, s_wire, wl);
        ca_snprintf(d, sizeof(d), "rc=%d", (int)rc);
        phase("reflection", rc == CA_SECURE_PIPE_DECRYPT, d);
    }

    /* peer_restart: role 2 comes back; the old session is gone */
    {
        u32 wl = 0U;
        (void)seal(&s_l1, s_wire2, &wl, 50U, 13U);             /* old session */
        ca_sp_link_deinit(&s_l2);
        bool re = ca_sp_link_init(&s_l2, 2U, PASSWORD, LINK_NAME) == CA_SECURE_PIPE_OK;
        /* this time role 2 starts */
        bool hs = re && handshake(&s_l2, &s_l1);
        i32  ro = hs ? open_(&s_l2, s_wire2, wl) : 0;
        bool rt = hs && roundtrip(&s_l1, &s_l2, 200U, 14U) && roundtrip(&s_l2, &s_l1, 200U, 15U);
        ca_snprintf(d, sizeof(d), "handshake %s, old msg rc=%d, new session %s",
                    hs ? "ok" : "FAIL", (int)ro, rt ? "ok" : "FAIL");
        phase("peer_restart", hs && ro == CA_SECURE_PIPE_DECRYPT && rt, d);
    }

end:
    print_stats("end1", &s_l1);
    print_stats("end2", &s_l2);
    ca_sp_link_deinit(&s_l1);
    ca_sp_link_deinit(&s_l2);
    ca_memzero(s_plain, sizeof(s_plain));
    ca_memzero(s_out, sizeof(s_out));
    ca_stdout_printf_n("LINK RESULT %s %u/%u", (s_pass == s_total && s_total > 0U) ?
                       "PASS" : "FAIL", (unsigned)s_pass, (unsigned)s_total);
}

static u32 task_link(void *arg)
{
    (void)arg;
    ca_os_sleep(300);
    ca_stdout_put_n("=== EX97: secure pipe link -- 'link test' to start ===");
    while (1) {
        ca_sem_wait(&s_go);
        s_running = true;
        run_tests();
        s_running = false;
    }
    return 0;
}

static u32 task_hb(void *arg)
{
    (void)arg;
    while (1) { ca_led_toggle(1U); ca_os_sleep(500); }
    return 0;
}

/* ============================================================================
 * CLI: link test
 * ============================================================================ */
#if ENABLE_CLI
static bool link_cli_handler(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) {
        CLI_PUT_N("link      : test (secure pipe link, both ends here)");
        return false;
    }
    if (ca_strncmp(p, "link", 4) != 0) return false;
    p = ca_skip_sp(p + 4);
    if (ca_strncmp(p, "test", 4) == 0) {
        if (s_running) { CLI_PUT_N("link: test already running"); return true; }
        ca_sem_post(&s_go);
        return true;
    }
    CLI_PUT_N("link: test");
    return true;
}
static ca_cli_node_t s_link_cli = { link_cli_handler, NULL };
#endif

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
        ca_cli_register(&s_link_cli);
    )
    ca_led_init();
    ca_led_off(1U);

    if (!ca_aes_init()) CA_SAFE_FAIL("ca_aes_init");
    if (!ca_rng_init())  CA_SAFE_FAIL("ca_rng_init");
    /* 4 slots per end (2 static + 2 session keys), three ends at most */
    if (ca_keys_store_init(12) != 0) CA_SAFE_FAIL("ca_keys_store_init");
    ca_sem_init(&s_go, "link_go", 0U, 1U);
}

void ca_os_main(void)
{
    ca_os_job("link", CA_TH_LEVEL3, task_link, NULL, STACK_LARGE, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_job("hb",   CA_TH_LEVEL3, task_hb,   NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);
}

#else

/* Pre-scheduler diagnostic: ca_stdout_* needs a task context (TLB=NULL before
 * ca_os_run), so print straight to the console backend with PUT_N. */
PRIVATE_STRING(m97a, "=== Example 97 ===");
PRIVATE_STRING(m97b, "needs EN_AES / EN_RNG / EN_KEYS_STORE / EN_AES_CHANNEL_SERVICE / EN_SHA256");
void ca_os_configure(void)
{
    /* The line the bench matches to know this example cannot run on
       this part. The reason follows it. */
    CA_EXAMPLE_UNSUPPORTED();
    PUT_N(&priv_m97a);
    PUT_N(&priv_m97b);
}
void ca_os_main(void) { ca_os_run(true); }

#endif
