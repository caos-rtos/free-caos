// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/**
 ******************************************************************************
 * @file           : ca_os_ex98.c
 * @brief          : Example 98 — CORDIC coprocessor (H563, U575)
 ******************************************************************************
 * @details
 * The board has no libm worth trusting as a reference, so the check is split:
 *
 *   cordic test   on the board, no floating point: identities that hold
 *                 whatever the reference is (sin^2+cos^2 = 1, sqrt(x)^2 = x,
 *                 polar -> rectangular -> polar), plus the cycles per call.
 *                 Ends with "CRD TEST PASS" or "CRD TEST FAIL".
 *   cordic dump   every function over the ranges of the reference manual
 *                 (RM0456 25.3.2), one line per operation with arguments
 *                 and raw results:  CRD <func> <scale> <arg1> <arg2> <res1> <res2>
 *                 (hex q1.31), closed by "CRD END <n>".
 *
 * The PC counterpart, cordic_check.py in this directory, runs "cordic dump"
 * through caos_shell and compares every result with Python's math module,
 * within the error bound of the reference manual for each function.
 *
 * On a part without CORDIC (F4, F7, H7) the example says so and stops.
 ******************************************************************************
 */

#include "caos.h"

#define EX98_POINTS     16U      /* operations per row of the dump */
#define EX98_TEST_N     256U     /* operations per identity in "cordic test" */
#define EX98_BENCH_N    1000U

/* q1.31 constants */
#define Q31(x)          ((i32)((x) * 2147483648.0))
#define Q31_ONE         0x7FFFFFFF

/* One row of the dump: a function, its scale, and a linear sweep of the
 * arguments from lo to hi (arg2 sweeps too, so the two-argument functions
 * do not see a single vector length). Ranges from RM0456 tables 197-208. */
typedef struct {
    ca_cordic_func_t func;
    u32 scale;
    i32 a1_lo, a1_hi;
    i32 a2_lo, a2_hi;
} ex98_row_t;

static const ex98_row_t s_rows[] = {
    { CA_CORDIC_COS,     0U, Q31(-1.0),    Q31(0.999),   Q31(0.5),  Q31_ONE    },
    { CA_CORDIC_SIN,     0U, Q31(-1.0),    Q31(0.999),   Q31_ONE,   Q31(0.5)   },
    { CA_CORDIC_PHASE,   0U, Q31(-0.7),    Q31(0.7),     Q31(0.6),  Q31(0.5)   },
    { CA_CORDIC_PHASE,   0U, Q31(-0.5),    Q31(-0.65),   Q31(-0.7), Q31(0.7)   },
    { CA_CORDIC_MODULUS, 0U, Q31(-0.7),    Q31(0.7),     Q31(-0.6), Q31(-0.5)  },
    { CA_CORDIC_MODULUS, 0U, Q31(0.5),     Q31(0.65),    Q31(0.7),  Q31(-0.7)  },
    { CA_CORDIC_ATAN,    0U, Q31(-1.0),    Q31(0.999),   0,         0          },
    { CA_CORDIC_ATAN,    3U, Q31(-1.0),    Q31(0.999),   0,         0          },
    { CA_CORDIC_COSH,    1U, Q31(-0.559),  Q31(0.559),   0,         0          },
    { CA_CORDIC_SINH,    1U, Q31(-0.559),  Q31(0.559),   0,         0          },
    { CA_CORDIC_ATANH,   1U, Q31(-0.403),  Q31(0.403),   0,         0          },
    { CA_CORDIC_LN,      1U, Q31(0.0535),  Q31(0.4999),  0,         0          },
    { CA_CORDIC_LN,      2U, Q31(0.25),    Q31(0.7499),  0,         0          },
    { CA_CORDIC_LN,      3U, Q31(0.375),   Q31(0.8749),  0,         0          },
    { CA_CORDIC_LN,      4U, Q31(0.4375),  Q31(0.584),   0,         0          },
    { CA_CORDIC_SQRT,    0U, Q31(0.027),   Q31(0.7499),  0,         0          },
    { CA_CORDIC_SQRT,    1U, Q31(0.375),   Q31(0.8749),  0,         0          },
    { CA_CORDIC_SQRT,    2U, Q31(0.4375),  Q31(0.585),   0,         0          },
};
#define EX98_ROWS  (sizeof(s_rows) / sizeof(s_rows[0]))

static i32 ex98_lerp(i32 lo, i32 hi, u32 i, u32 n)
{
    return (i32)((i64)lo + (((i64)hi - (i64)lo) * (i64)i) / (i64)(n - 1U));
}

static i32 ex98_abs(i32 v) { return (v < 0) ? -v : v; }

/* q1.31 product, i64 so that nothing overflows */
static i64 ex98_mul(i32 a, i32 b) { return ((i64)a * (i64)b) >> 31; }

static void ex98_dump(void)
{
    u32 n = 0U;
    for (u32 r = 0U; r < EX98_ROWS; r++) {
        const ex98_row_t *row = &s_rows[r];
        for (u32 i = 0U; i < EX98_POINTS; i++) {
            const i32 a1 = ex98_lerp(row->a1_lo, row->a1_hi, i, EX98_POINTS);
            const i32 a2 = ex98_lerp(row->a2_lo, row->a2_hi, i, EX98_POINTS);
            i32 r1 = 0, r2 = 0;
            (void)ca_cordic_calc(row->func, row->scale, a1, a2, &r1, &r2);
            CLI_PRINTF_N("CRD %u %u %08X %08X %08X %08X", (u32)row->func, row->scale,
                         (u32)a1, (u32)a2, (u32)r1, (u32)r2);
            n++;
        }
    }
    CLI_PRINTF_N("CRD END %u", n);
}

/* Worst deviation of each identity, in units of 2^-31 */
static void ex98_test(void)
{
    i32 worst_sc = 0, worst_sq = 0, worst_pa = 0, worst_pm = 0;

    for (u32 i = 0U; i < EX98_TEST_N; i++) {
        /* sin^2 + cos^2 = 1 over the whole circle */
        const i32 ang = ex98_lerp(Q31(-1.0), Q31(0.999), i, EX98_TEST_N);
        i32 s = 0, c = 0;
        (void)ca_cordic_sincos(ang, &s, &c);
        const i64 one = ex98_mul(s, s) + ex98_mul(c, c);
        const i32 d_sc = ex98_abs((i32)(one - (i64)Q31_ONE));
        if (d_sc > worst_sc) worst_sc = d_sc;

        /* polar -> rectangular -> polar: (m cos a, m sin a) gives back a and m */
        const i32 m = Q31(0.8);
        i32 x = 0, y = 0, a = 0, mm = 0;
        (void)ca_cordic_calc(CA_CORDIC_COS, 0U, ang, m, &x, &y);
        (void)ca_cordic_polar(x, y, &a, &mm);
        i32 d_pa = ex98_abs((i32)((u32)a - (u32)ang));   /* wraps at +-pi */
        if (d_pa > worst_pa) worst_pa = d_pa;
        const i32 d_pm = ex98_abs(mm - m);
        if (d_pm > worst_pm) worst_pm = d_pm;

        /* sqrt(x)^2 = x, scale 0 range */
        const i32 v = ex98_lerp(Q31(0.027), Q31(0.7499), i, EX98_TEST_N);
        i32 rt = 0;
        (void)ca_cordic_calc(CA_CORDIC_SQRT, 0U, v, 0, &rt, NULL);
        const i32 d_sq = ex98_abs((i32)(ex98_mul(rt, rt) - (i64)v));
        if (d_sq > worst_sq) worst_sq = d_sq;
    }

    /* Bounds: 24 iterations leave 2^-19 per result plus 2^-20 rounding
     * (RM0456 table 209); the identities combine two or three results. */
    const i32 tol = 1 << 14;     /* 2^-17 */
    const bool ok = worst_sc < tol && worst_pa < tol && worst_pm < tol && worst_sq < tol;

    CLI_PRINTF_N("CRD sin^2+cos^2-1   worst %u / 2^31", (u32)worst_sc);
    CLI_PRINTF_N("CRD polar angle     worst %u / 2^31", (u32)worst_pa);
    CLI_PRINTF_N("CRD polar modulus   worst %u / 2^31", (u32)worst_pm);
    CLI_PRINTF_N("CRD sqrt(x)^2-x     worst %u / 2^31", (u32)worst_sq);
    CLI_PRINTF_N("CRD tolerance       %u / 2^31", (u32)tol);

    /* Cost of one call as an application sees it: argument checks,
     * critical section, CSR, write, stalled read */
    i32 s = 0, c = 0;
    const u32 t0 = DWT->CYCCNT;
    for (u32 i = 0U; i < EX98_BENCH_N; i++)
        (void)ca_cordic_sincos((i32)(i << 20), &s, &c);
    const u32 cyc = (DWT->CYCCNT - t0) / EX98_BENCH_N;
    CLI_PRINTF_N("CRD sincos %u cycles per call (%u MHz)", cyc, SystemCoreClock / 1000000U);

    CLI_PUT_N(ok ? "CRD TEST PASS" : "CRD TEST FAIL");
}

#if ENABLE_CLI
static bool ex98_cli(c_str *pcmd)
{
    const char *p = ca_skip_sp((*pcmd)->data);
    if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("cordic: test | dump"); return false; }
    if (ca_strncmp(p, "cordic", 6) != 0) return false;
    p = ca_skip_sp(p + 6);
    if (ca_strncmp(p, "test", 4) == 0) { ex98_test(); return true; }
    if (ca_strncmp(p, "dump", 4) == 0) { ex98_dump(); return true; }
    CLI_PUT_N("cordic: test | dump");
    return true;
}
static ca_cli_node_t s_cli = { ex98_cli, NULL };
#endif

PRIVATE_STRING(why, "ex98: this part has no CORDIC coprocessor (H563 and U575 have it)");

void ca_os_configure(void)
{
    if (!ca_cordic_init()) {
        /* The line the bench matches to know this example cannot run on
           this part. The reason follows it. */
        CA_EXAMPLE_UNSUPPORTED();
        PUT_N(&priv_why);
        return;
    }

    EVAL_STDOUT(
        ca_stdout_init();
        ca_stdout_set_sender(STDOUT);
        ca_stdout_create_thread();
    )
    EVAL_CLI(
        ca_cli_init(2048);
        ca_cli_set_sender(STDOUT);
        ca_cli_create_thread();
        ca_cli_register(&s_cli);
    )
}

void ca_os_main(void)
{
    ca_os_run(true);
}
