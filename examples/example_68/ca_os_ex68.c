// Proprietary — All rights reserved
// Copyright (c) 2026 Angelo Coppi
// Coppi Angelo Operating System
// caos.rtos@protonmail.com

/*
 * Example 68 — Fault / exception test bench (MPU, Bus, Usage faults)
 *
 * Deliberately provokes CPU faults, one at a time, from a CLI command. Each
 * offending action runs in its own task created with CA_FAULT_ASSERT, so after
 * the fault the safeguard REMOVES the task and the CPU recovers (a deterministic
 * fault would just re-fault on restart). Lets us observe the fault decode
 * (os_handlers.c) and verify the system keeps running afterwards.
 *
 * The console (STDIN/STDOUT) is whatever os_config selects — not touched here.
 *
 * CLI commands (type 'help'):
 *   fault null_r    — read  *(u32*)0   -> M33: fault; M4/M7: NO fault (flash aliased at 0)
 *   fault null_w    — write *(u32*)0   -> M33: MemManage/BusFault; M4/M7: NO fault (write absorbed by locked FPEC)
 *   fault ram       — access beyond RAM end   -> BusFault
 *   fault flash     — read beyond Flash end   -> BusFault
 *   fault ibuserr   — fetch past flash end     -> BusFault IBUSERR
 *   fault ro        — write to own stack canary (RO) -> MemManage DACCVIOL
 *   fault stack     — write below stack_end (canary) -> MemManage DACCVIOL
 *   fault stackjmp  — write far below stack_end (jump 32B band) -> MemManage DACCVIOL
 *   fault stackup   — corrupt another task's saved PSP upward -> killed by PSP range-check (not a CPU fault)
 *   fault xn        — execute from a data buffer COVERED by an XN region
 *                     -> MemManage IACCVIOL on the first fetch
 *   fault runaway   — execute from a data buffer with NO region: the twin of
 *                     'xn', and the difference between them is the lesson.
 *                     The fetch is LEGAL (PRIVDEFENA + privileged + SRAM is
 *                     not execute-never in the ARM default map), the zeroed
 *                     buffer is eight valid NOPs, and the core runs on through
 *                     .bss until a bit pattern stops it -> UsageFault, of
 *                     varying kind, at a varying address. Measured on F446:
 *                     UNDEFINSTR @0x2000F118 x3, UNALIGNED @0x2000F64C x1
 *   fault div0      — divide by zero   -> UsageFault DIVBYZERO
 *   fault unalign   — unaligned u32 access -> UsageFault UNALIGNED
 *   fault undef     — undefined instruction -> UsageFault UNDEFINSTR
 *   fault invstate  — branch to non-Thumb addr -> UsageFault INVSTATE
 *   fault forced    — USGFAULTENA off + div0 -> escalated HardFault (FORCED), downgraded to per-task remove
 *   fault fp        — BusFault with the FP context ARMED -> the dump must read
 *                     EXC_RETURN=0xFFFFFFAC (extended frame), not ...BC
 *   fault fpctx     — r4-r6/r8/s16 across 6 context switches while another task
 *                     hammers S16/S17: prints PASS or FAIL, provokes no fault
 *                     (both only where the FPU is present AND used)
 *
 * Kernel safeguards (CLI only, not in the automatic sequence):
 *   fault safeguard — CA_SAFE_FAIL in thread mode -> [SAFEGUARD] line, SVC
 *                     served, task removed, kernel alive
 *   fault sg_irq    — ca_mutex_unlock on a mutex nobody owns: the check runs
 *                     under PRIMASK -> [SAFEGUARD] line, then HardFault FORCED
 *                     with CFSR=0 (not recoverable) -> RESET
 *   fault irq       — div0 inside the TIM3 update ISR (EN_TIMER): no task to
 *                     blame -> 'Faulted in handler mode', system HALTED, no
 *                     task removed
 *
 * Allocator (CLI only, leaves the heap corrupted until the next reset):
 *   fault restart   — div0 in a CA_FAULT_RESTART_THREAD task, on its FIRST run
 *                     only: the task must be renewed (ca_thread_renew re-arms
 *                     the stack guard) and run again, "alive after restart,
 *                     canary OK". The only case that takes the RESTART path;
 *                     on M33 that path once gave IACCVIOL (TODO §1).
 *   fault heap      — user data over a block header, then free it and exit:
 *                     'Invalid Free' + 'heap chain corrupted', NO fault
 *
 * Expected: every fault is decoded by os_handlers.c, the offending task is
 * removed, and the system keeps running (monitor 'EX68 alive' continues).
 */

#include "caos.h"

/* Out-of-bounds addresses just past the END of the real memory of THIS MCU
 * (selected by MCU_ID). A fixed "big" value would still be mapped on parts with
 * more RAM/flash (F407: 128K/512K vs U575: 628K/2MB), so the end must be
 * per-family. RAM base 0x20000000, Flash base 0x08000000 on all these parts. */
#if   MCU_ID == 407 || MCU_ID == 429 || MCU_ID == 446
  #define ADDR_BEYOND_RAM     0x20040000UL   /* F4: up to 256K SRAM → past end */
  #define ADDR_BEYOND_FLASH   0x08100000UL   /* F4: up to 1M flash  → past end */
#elif MCU_ID == 103
  #define ADDR_BEYOND_RAM     0x20018000UL   /* F103: 96K SRAM      → past end */
  #define ADDR_BEYOND_FLASH   0x08080000UL   /* F103: 512K flash    → past end */
#elif MCU_ID == 746
  #define ADDR_BEYOND_RAM     0x20080000UL   /* F746: 320K SRAM     → past end */
  #define ADDR_BEYOND_FLASH   0x08100000UL   /* F746: 1M flash      → past end */
#elif MCU_ID == 575
  /* U575 has several SRAM banks beyond the 628K the linker uses (SRAM up to
   * ~0x200C0000 + backup SRAM4), so 'just past linker end' can still be mapped.
   * Use a clearly unmapped address above all SRAM banks. */
  #define ADDR_BEYOND_RAM     0x20100000UL   /* U575: above all SRAM → unmapped */
  #define ADDR_BEYOND_FLASH   0x08200000UL   /* U575: 2M flash       → past end */
#elif MCU_ID == 563
  /* H563 -> 0x20050000 does NOT fault: on H5 the SRAM banks (SRAM1/2/3) are
   * contiguous from 0x20000000 up to ~0x200A0000, so 0x20050000 is still mapped
   * RAM and the write is absorbed with no BusFault. To actually trap on H563 the
   * address must land ABOVE all SRAM banks (e.g. 0x200A0000) or in an unmapped
   * hole. Left as-is for now: on H563 the 'ram' case is simply a no-op. */
  #define ADDR_BEYOND_RAM     0x20050000UL   /* H563: still mapped SRAM → NO fault */
  #define ADDR_BEYOND_FLASH   0x08200000UL   /* H563: 2M flash      → past end */
#elif MCU_ID == 753 || MCU_ID == 747
  #define ADDR_BEYOND_RAM     0x24080000UL   /* H7: AXI SRAM 512K   → past end */
  #define ADDR_BEYOND_FLASH   0x08200000UL   /* H7: 2M flash        → past end */
#else
  #define ADDR_BEYOND_RAM     0x60000000UL   /* fallback: unmapped FSMC region */
  #define ADDR_BEYOND_FLASH   0x3F000000UL   /* fallback: reserved hole        */
#endif

/* Help suffix for the FP cases. Spelled as a macro because CLI_PUT_N is
 * itself a function-like macro, and a #if inside a macro's argument list is
 * not portable C - string-literal concatenation after expansion is. */
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
  #define EX68_FP_HELP  " | fp | fpctx"
#else
  #define EX68_FP_HELP  ""
#endif

/* Keep the compiler from optimizing the offending accesses away. */
static volatile u32 g_sink;

/* The UNGUARDED buffer. The comment that used to sit here said RAM "is
 * Execute-Never under the MPU config" - it is not, and that false premise is
 * what made the 'xn' case look like it proved something. See task_runaway.
 * Also used by the unalign and invstate cases, which only need an address. */
static volatile u8 g_data_buf[16] = { 0 };

/* The GUARDED buffer, and the region that guards it.
 *
 * 32 bytes, 32-byte aligned, because that satisfies both architectures at once:
 * ARMv7-M wants a power-of-two size with the base aligned to it, ARMv8-M wants
 * base and limit on a 32-byte granule.
 *
 * ca_mpu_add_region() sets XN=1 on every region it creates, on both (see
 * hw/f4xx/common/mpu.c and hw/u5xx/common/mpu.c), so MPU_ACCESS_RDWR here
 * means "data yes, instructions no" - which is exactly the one thing this case
 * has to be able to say. */
static volatile u8 g_xn_buf[32] __attribute__((aligned(32))) = { 0 };

/* Deliberately declared even where there is no MPU: the case then reports
 * itself SKIPPED, which is a fact about the part. Compiled out instead, it
 * would silently disappear from a bench whose whole job is to be complete. */
static int g_xn_region = -1;

/* ============================================================================
 * FAULT-PROVOKING WORKERS (each runs as a restartable task)
 * ========================================================================== */

/* Simulate the stack descending into the neighbouring task's area: write the
 * bytes just below stack_end, i.e. the MPU canary region (stack_end - CANARY).
 * That is exactly the boundary the MPU protects, so the write triggers a clean
 * MemManage (DACCVIOL, HIT LOWER CANARY) that the kernel recovers from.
 *
 * NOTE: a real infinite recursion is NOT used here — the compiler's per-frame
 * push can jump OVER the 32-byte canary and land in the neighbour's stack,
 * shredding this task's own return frame before any handler runs (→ HardFault
 * INVSTATE, unrecoverable). A controlled write hits the protection deterministic-
 * ally. (task_ro already covers writing the canary from within the task.) */

/* Victim task for the stack-underflow test: it yields in a loop so it stays
 * READY and gets re-scheduled quickly. Between yields it is parked with its PSP
 * saved in its context (TLB area); task_stackup corrupts that saved PSP from the
 * outside, and the victim is removed the next time the scheduler tries to mount
 * it (incoming-task range-check) — so this loop never actually spins forever. */
static u32 task_victim(void *arg) {
    (void)arg;
    for (u32 i = 0; i < 100000; i++) {
        ca_os_yield();
    }
    return 0;
}

/* Stack UNDERFLOW (overflow upward), from ANOTHER task: the MPU guard protects
 * BELOW stack_end and never sees a PSP that rose ABOVE stack_start. Nor is this
 * a memory-access fault — it is a corrupted SAVED PSP. Reproduced the honest
 * way: spawn a victim that parks (asleep → its PSP lives in its context), then
 * reach into the task table, find the victim by name and overwrite its saved
 * ctx->psp to stack_start + 64 (out of range, upward). When the scheduler next
 * tries to mount the victim, the INCOMING-task range-check in the context switch
 * catches psp > stack_start and removes the VICTIM before restoring registers
 * from an illegal PSP — no HardFault, no collateral corruption. */
static u32 task_stackup(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: corrupt ANOTHER task's saved PSP (upward)");

    ca_os_job("f_victim", CA_TH_LEVEL3, task_victim, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
    ca_os_sleep(50);   /* let the victim start and park (PSP saved in context) */

    u32 n = ca_scheduler_size();
    for (u32 i = 0; i < n; i++) {
        ca_thread_context_t *v = (ca_thread_context_t *)ca_scheduler_get_context_from_idx(i);
        if (v && ca_strncmp(v->name, "f_victim", 8) == 0) {
            /* Corrupt the SAVED PSP upward, out of [stack_end, stack_start]. */
            v->psp = v->stack_start + 64;
            ca_stdout_printf_n("[EX68] victim '%s' psp forced to 0x%08X (stack_start=0x%08X)",
                               v->name, v->psp, v->stack_start);
            break;
        }
    }
    /* This task returns normally; the victim dies on its next scheduling. */
    return 0;
}

/* CROSS-TASK write: reach into ANOTHER task's stack, from this task. This is
 * the ex37 dns_service/arp_service bug reproduced deliberately — a service
 * storing a caller's pointer and writing through it from its own task.
 * The point of the case is the DIAGNOSIS: the area below our stack_end is
 * exactly where the other tasks' stacks live, so the DACCVIOL is identical to
 * the one a real overflow produces. The handler must name the victim instead
 * of reporting an overflow of this task that never happened.
 *
 * IMPORTANT — the target cannot be picked freely. The MPU guard covers a
 * limited window below our stack_end (on ARMv7-M a power-of-2 block ending at
 * the canary, so 4-8KB with typical stacks; the full span only on ARMv8-M).
 * A victim BELOW that window is not protected and the write lands SILENTLY,
 * corrupting its stack — which then surfaces much later as a bogus "stack
 * overflow" from the watermark check. So: scan the task list and pick a victim
 * whose stack actually intersects the guarded window. */
static u32 task_cross(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: write into ANOTHER task's stack (cross-task)");

    const ca_thread_context_t *me = ca_scheduler_get_current_context();
    if (me == NULL) return 0;

    /* Lower bound of the guarded window: largest power-of-2 block ending at our
     * canary (mirrors mpu_protect_stack_deep_guard); on full-span guards the
     * whole area below is covered anyway, so this stays conservative. */
    u32 canary = me->stack_end - STACK_CANARY_SIZE;
    u32 span   = canary & (~canary + 1U);
    u32 floor  = canary - span;
    u32 arena  = (u32)&_psp_stack_base;
    if (floor < arena) floor = arena;

    u32 n = ca_scheduler_size();
    for (u32 i = 0; i < n; i++) {
        const ca_thread_context_t *v = ca_scheduler_get_context_from_idx(i);
        if (v == NULL || v == me) continue;
        /* Victim's stack must overlap [floor, canary): only there is the write
         * actually trapped rather than silently absorbed. */
        if (v->stack_start <= floor || v->stack_end >= canary) continue;

        u32 lo = (v->stack_end   > floor)  ? v->stack_end   : floor;
        u32 hi = (v->stack_start < canary) ? v->stack_start : canary;
        u32 target = (lo + ((hi - lo) / 2)) & ~3U;
        ca_stdout_printf_n("[EX68] writing into '%s' stack @ 0x%08X", v->name, target);
        *(volatile u32 *)target = 0xC0DEFEED;   /* MemManage DACCVIOL */
        return 0;
    }

    ca_stdout_put_n("[EX68] cross: no victim stack inside the guarded window (skipped)");
    return 0;
}

static u32 task_stack(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: stack-into-neighbour (write below stack_end)");
    const ca_thread_context_t *ctx = ca_scheduler_get_current_context();
    if (ctx) {
        /* stack_end - STACK_CANARY_SIZE = the MPU-protected boundary the stack
         * would cross when it overflows into the neighbouring task. */
        volatile u32 *below = (volatile u32 *)(ctx->stack_end - STACK_CANARY_SIZE);
        *below = 0xDEADBEEF;     /* MemManage DACCVIOL — HIT LOWER CANARY */
    }
    return 0;
}

/* Jump OVER the 32-byte canary band: write well below stack_end, into the area
 * a large single frame (e.g. u8 buf[400]) would reach in one stride, skipping
 * the band entirely. A 32-byte-only guard does NOT cover this (RW memory → the
 * write lands silently). The full-span stack guard protects the whole region
 * below stack_end, so this faults cleanly (MemManage DACCVIOL).
 * A pointer write (not real SP descent) keeps PSP valid → recoverable fault. */
static u32 task_stackjmp(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: stack JUMP over 32B band (write far below stack_end)");
    const ca_thread_context_t *ctx = ca_scheduler_get_current_context();
    if (ctx) {
        u32 band   = ctx->stack_end - STACK_CANARY_SIZE;  /* canary base */
        u32 target = band - 256;                          /* 256B below the band */
        u32 arena  = (u32)&_psp_stack_base;               /* keep inside the guard */
        if (target < arena) target = arena;
        *(volatile u32 *)target = 0xBADC0FFE;    /* below band: silent on old guard */
    }
    return 0;
}

static u32 task_null_r(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: read from NULL (0x0)");
    /* The 0x0 read itself does NOT fault on M4/M7 (flash aliased at 0x0), but
     * dereferencing NULL is UB: the compiler emits a `udf` right after under -O,
     * so this still traps as UsageFault UNDEFINSTR (PC = the udf). On M33 the
     * 0x0 access faults first (BusFault/MemManage). */
    g_sink += *(volatile u32 *)0;
    ca_stdout_put_n("[EX68] null_r: no fault (address 0x0 readable here)");
    return 0;
}

static u32 task_null_w(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: write to NULL (0x0)");
    /* Faults on M33 (U5/H5: 0x0 is RO/unmapped) but NOT on M4/M7 (F4/F7):
     * there BOOT0=0 aliases main Flash at 0x0, and a write with FPEC locked
     * (LOCK=1, the idle state) is absorbed by the Flash interface without a
     * bus error — same reason null_r reads 0x0 cleanly. Not a kernel defect. */
    *(volatile u32 *)0 = 0xDEADBEEF;
    ca_stdout_put_n("[EX68] null_w: no fault here (0x0 flash-aliased, write absorbed)");
    return 0;
}

static u32 task_ram(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: access beyond RAM end");
    *(volatile u32 *)ADDR_BEYOND_RAM = 0x12345678;
    /* M7: the store is buffered and the BusFault is IMPRECISE. Without the
     * barrier the task can return first and the fault lands in PendSV
     * (handler mode, no task to blame -> halt): seen on H753 once the
     * I-cache made the exit path fast enough. DSB drains it here. */
    __DSB();
    return 0;
}

static u32 task_flash(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: read beyond Flash end");
    g_sink += *(volatile u32 *)ADDR_BEYOND_FLASH;
    return 0;
}

/* 'runaway' and 'xn' are a PAIR, and the difference between them is the point.
 *
 * Both branch into a zeroed data buffer in .bss. One of them is covered by an
 * MPU region with XN=1 and one is not, and that single difference decides
 * whether the core faults on the first instruction fetch or executes the data.
 *
 * MEASURED on F446 18/09, four runs of the uncovered one: UNDEFINSTR at
 * 0x2000F118 three times, UNALIGNED at 0x2000F64C once. Never IACCVIOL. In
 * Thumb, 0x0000 is LSLS r0,r0,#0 - a VALID instruction - so a zeroed buffer is
 * not a trap, it is eight NOPs followed by whatever .bss holds next. The core
 * ran 200 bytes past the end of the buffer (1532 on one run) and was stopped
 * by the bit pattern of g_cur_index, a live scheduler variable. Where it stops
 * depends on kernel state at that instant, which is why the fault changes from
 * run to run.
 *
 * Executing from .bss is LEGAL here and that is not a defect: MPU->CTRL has
 * PRIVDEFENA, the tasks are privileged, and the ARM default map does not mark
 * SRAM execute-never. Which also settles the question this pair was written to
 * answer: yes, the heap is executable on these parts unless a region says no.
 */
static u32 task_runaway(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: execute from data buffer, NO MPU region");
    /* The address is printed because it is the criterion: a fault AT this
     * address would mean the fetch was refused, which is the twin's job. What
     * this case claims is a fault PAST it. Where past it depends on what is
     * next in .bss - once the 'xn' region exists it is usually that region, a
     * few tens of bytes up, which is itself a demonstration that the guard
     * works. Before it existed, on F446, the core got 200 bytes further, into
     * the scheduler's own variables. */
    ca_stdout_printf_n("[EX68] buffer at 0x%08X, not covered - expect the core to RUN ON",
                       (u32)g_data_buf);
    /* bit0=1 → Thumb state, so nothing here is a state fault. BLX on a
     * register: an indirect branch, no linker relocation. */
    register u32 target __asm__("r0") = (u32)g_data_buf | 1U;
    __asm volatile ("blx %0" : : "r"(target) : "lr", "cc", "memory");
    return 0;
}

static u32 task_xn(void *arg) {
    (void)arg;
    /* Without the region this case is the runaway one wearing its name - it
     * would still "fault", the bench would still count a RECOVER, and the
     * verdict would be wrong in exactly the way that kept this open for weeks.
     * So it refuses to run rather than pass for the wrong reason. */
    if (g_xn_region < 0) {
        ca_stdout_put_n("[EX68] SKIPPED: no MPU region guards the buffer - "
                        "without it this case cannot tell XN from a runaway");
        return 0;
    }
    ca_stdout_put_n("[EX68] provoking: execute from data buffer under an XN region");
    ca_stdout_printf_n("[EX68] buffer at 0x%08X, MPU region %d, XN=1 - expect IACCVIOL "
                       "on the FIRST fetch", (u32)g_xn_buf, g_xn_region);
    register u32 target __asm__("r0") = (u32)g_xn_buf | 1U;
    __asm volatile ("blx %0" : : "r"(target) : "lr", "cc", "memory");
    return 0;
}

static u32 task_ro(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: write to own stack canary (RO)");
    const ca_thread_context_t *ctx = ca_scheduler_get_current_context();
    if (ctx) {
        /* Lower canary sits at stack_end - STACK_CANARY_SIZE, protected RO. */
        volatile u32 *canary = (volatile u32 *)(ctx->stack_end - STACK_CANARY_SIZE);
        *canary = 0x0BADF00D;                /* DACCVIOL on RO region */
    }
    return 0;
}

/* Restart case: faults on the first run only, so the renewed task has a
 * second life in which to prove the restart path works. Counter reset by the
 * CLI command, so the case can be repeated. */
static volatile u32 g_restart_runs;

static u32 task_restart(void *arg) {
    (void)arg;
    u32 run = ++g_restart_runs;
    const ca_thread_context_t *ctx = ca_scheduler_get_current_context();
    ca_stdout_printf_n("[EX68] restart: run %u (n_restart=%u)",
                       run, ctx ? (u32)ctx->n_restart : 0U);
    if (run == 1U) {
        ca_stdout_put_n("[EX68] restart: provoking divide by zero, expect RESTART");
        volatile u32 a = 100, b = 0;
        g_sink += a / b;                     /* UsageFault DIVBYZERO */
        return 0;
    }
    /* Second life: the lower stack guard ca_thread_renew rewrote and re-armed
     * must hold the pattern. Reading it is allowed (the region is RO). */
    bool canary_ok = false;
    if (ctx) {
        const volatile u64 *c = (const volatile u64 *)(ctx->stack_end - STACK_CANARY_SIZE);
        canary_ok = (c[0] == STACK_CANARY_VALUE) && (c[1] == STACK_CANARY_VALUE) &&
                    (c[2] == STACK_CANARY_VALUE) && (c[3] == STACK_CANARY_VALUE);
    }
    ca_stdout_printf_n("[EX68] restart: alive after restart, canary %s",
                       canary_ok ? "OK" : "BAD");
    return 0;
}

static u32 task_div0(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: divide by zero");
    volatile u32 a = 100, b = 0;
    g_sink += a / b;                         /* UsageFault DIVBYZERO */
    return 0;
}

static u32 task_unalign(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: unaligned u32 access");
    /* UNALIGN_TRP is off globally (would fault normal init) → enable it here so
     * the unaligned load below actually traps instead of being HW-fixed. */
    SCB->CCR |= SCB_CCR_UNALIGN_TRP_Msk;
    __DSB(); __ISB();
    volatile u32 *p = (volatile u32 *)((u32)&g_data_buf[1]);  /* odd address */
    g_sink += *p;                            /* UsageFault UNALIGNED */
    return 0;
}

static u32 task_undef(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: undefined instruction");
    /* 0xF7F0A000 = permanently UNDEFINED encoding (UDF.W). */
    __asm volatile (".inst.w 0xF7F0A000");   /* UsageFault UNDEFINSTR */
    return 0;
}

static u32 task_invstate(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: invalid state (branch to non-Thumb addr)");
    /* Branch to an address with bit0 = 0: the core tries to switch to ARM state
     * (unsupported on Cortex-M) → UsageFault INVSTATE. Use BLX on a register so
     * the assembler emits an indirect branch (a plain call to a data symbol
     * would produce an R_ARM_THM_CALL relocation the linker can't resolve). */
    register u32 target __asm__("r0") = (u32)g_data_buf & ~1U;
    __asm volatile ("blx %0" : : "r"(target) : "lr", "cc", "memory");
    return 0;
}

static u32 task_ibuserr(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: instruction bus error (fetch past flash)");
    /* Branch (Thumb, bit0=1) to an unmapped code address → the instruction FETCH
     * bus-errors → BusFault IBUSERR (distinct from a data BusFault). BLX on a
     * register: indirect branch, no linker relocation to the literal address. */
    register u32 target __asm__("r0") = ADDR_BEYOND_FLASH | 1U;
    __asm volatile ("blx %0" : : "r"(target) : "lr", "cc", "memory");
    return 0;
}

/* Escalated (forced) HardFault: disable the UsageFault handler in SHCSR, then
 * divide by zero. With USGFAULTENA off the configurable fault has no dedicated
 * handler and escalates to HardFault with HFSR.FORCED set, on the PSP frame,
 * CFSR.DIVBYZERO valid. This is the case the HardFault gate downgrades to the
 * per-task remove policy — the whole RTOS must NOT reset. The kernel re-enables
 * SHCSR ENA bits while recovering, so no cleanup is needed (nor reachable) here. */
/* ── FP context: the path the other sixteen cases never take ─────────────────
 *
 * The kernel decides whether to save/restore S16-S31 from EXC_RETURN bit 4
 * (FType), NOT from the task's declared JOB_ type - because with
 * -mfloat-abi=hard (every family here except F103) the compiler emits FP
 * instructions in ANY function, so a CA_JOB_STD task can carry an EXTENDED
 * exception frame without ever having asked for one.
 *
 * That was a real bug, found 2026-09-01 on U575 and H563 through ex59
 * "pwr stop": the kernel went by the declared type, the frames ended up 64
 * bytes out of step, and R4-R11 came back corrupted - r7, the frame pointer,
 * first. It faulted only when r7 happened to land on 0; the rest of the time it
 * wrote somewhere else and carried on.
 *
 * ⚠️ Every OTHER case in this bench faults with a BASIC frame
 * (EXC_RETURN=0xFFFFFFBC), which is why the bench read 16/16 for months while
 * the bug was live. A test bench that never enters a path cannot defend it.
 *
 *   fp     - fault WITH the FP context armed: the dump must read ...AC, not
 *            ...BC. Must be a BusFault: it is the only handler that prints
 *            EXC_RETURN.
 *   fpctx  - callee-saved integrity across real context switches, while a
 *            second task keeps overwriting the FP registers.
 */
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)

static u32 task_fp(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: BusFault with FP context ACTIVE"
                    " - expect EXC_RETURN=0xFFFFFFAC (extended frame)");
    volatile float f = 3.5f;
    f = f * 2.0f + 1.0f;                            /* arms the FP context */
    g_sink += (u32)f;
    g_sink += *(volatile u32 *)ADDR_BEYOND_FLASH;   /* BusFault PRECISERR */
    return 0;
}

static volatile bool s_fp_hammer;

/* Companion. Without something actively overwriting S16-S31, the check below
 * would assert the AAPCS contract in the abstract and could pass even with the
 * bug present: a kernel that never saves those registers loses nothing if
 * nobody else touches them. This task makes sure somebody does. */
static u32 task_fp_hammer(void *arg) {
    (void)arg;
    while (s_fp_hammer) {
        u32 junk = 0xBAD00000u;
        __asm__ volatile ("vmov s16, %0 \n vmov s17, %0"
                          :: "r" (junk) : "s16", "s17");
        (void)ca_os_sleep(1u);
    }
    return 0;
}

static u32 task_fpctx(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] checking: callee-saved regs across context switches,"
                    " FP armed, a second task hammering S16/S17");

    s_fp_hammer = true;
    ca_os_job("f_fphammer", CA_TH_LEVEL3, task_fp_hammer, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);

    u32 bad = 0u;
    for (u32 i = 0u; i < 6u && bad == 0u; i++) {
        /* volatile + "m": the operands live in MEMORY, not in registers.
         * With registers the block asked for two allocatable ones while
         * clobbering nearly the whole bank, and GCC answered "impossible
         * constraints" - r7 is the frame pointer and off limits, r9 can be
         * reserved. Memory operands are addressed through sp/r7, which the
         * block does not touch, so nothing has to be spared for them. */
        volatile u32 pat  = 0xC0DE0000u + i;
        volatile u32 diff = 0u;
        /* ONE asm block start to finish: load, survive a sleep (which forces a
         * real context switch), compare. Split across separate blocks the
         * compiler would be free to use these registers in between and the
         * test would measure nothing. r7 is absent on purpose: it is the frame
         * pointer, GCC will not let inline asm clobber it - and it is exactly
         * the register the original bug left at zero. r4-r6 and r8 share its
         * fate, so they stand in for it. */
        __asm__ volatile (
            "ldr   r0,  %[pat]    \n"
            "vmov  s16, r0        \n"
            "mov   r4,  r0        \n"
            "mov   r5,  r0        \n"
            "mov   r6,  r0        \n"
            "mov   r8,  r0        \n"
            "mov   r0,  #20       \n"
            "bl    ca_os_sleep    \n"
            "ldr   r0,  %[pat]    \n"
            "vmov  r1,  s16       \n"
            "eor   r1,  r1,  r0   \n"
            "eor   r2,  r4,  r0   \n"
            "orr   r1,  r1,  r2   \n"
            "eor   r2,  r5,  r0   \n"
            "orr   r1,  r1,  r2   \n"
            "eor   r2,  r6,  r0   \n"
            "orr   r1,  r1,  r2   \n"
            "eor   r2,  r8,  r0   \n"
            "orr   r1,  r1,  r2   \n"
            "str   r1,  %[dif]    \n"
            : [dif] "=m" (diff)
            : [pat] "m"  (pat)
            : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r8", "r12", "lr",
              "s16", "cc", "memory");
        if (diff != 0u) bad = i + 1u;
    }

    s_fp_hammer = false;
    if (bad != 0u)
        ca_stdout_printf_n("[EX68] FP CONTEXT: **FAIL** at round %u"
                           " - callee-saved state did not survive", bad);
    else
        ca_stdout_put_n("[EX68] FP CONTEXT: PASS"
                        " (r4-r6, r8 and s16 survived 6 context switches)");
    return 0;
}

#endif /* __FPU_PRESENT && __FPU_USED */

static u32 task_forced(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: escalated HardFault (USGFAULTENA off + div0)");
    SCB->CCR   |= SCB_CCR_DIV_0_TRP_Msk;      /* ensure /0 traps */
    SCB->SHCSR &= ~SCB_SHCSR_USGFAULTENA_Msk; /* no UsageFault handler -> escalate */
    __DSB(); __ISB();
    volatile u32 a = 100, b = 0;
    g_sink += a / b;                          /* HardFault FORCED (CFSR.DIVBYZERO) */
    return 0;
}

/* ============================================================================
 * KERNEL SAFEGUARDS — not CPU faults
 *
 * CA_SAFEGUARD is an assertion inside the kernel. On failure
 * ca_safeguard_failed() prints condition/file/line/task and raises
 * SVC_SAFEGUARD, whose handler removes the task. None of the cases above ever
 * went through it. CLI only, NOT in the automatic sequence: sg_irq resets the
 * part.
 * ========================================================================== */

/* Thread mode, IRQs enabled: the SVC is served, the task is removed and the
 * kernel goes on. The clean path. */
static u32 task_safeguard(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: kernel safeguard with IRQs enabled (SVC served)");
    ca_os_sleep(100);                         /* let the line out first */
    CA_SAFE_FAIL("ex68 deliberate safeguard");
    return 0;
}

static ca_mutex_t s_sg_mtx;

/* ============================================================================
 * ALLOCATOR — a corrupted block header must be refused, not followed
 *
 * ex89 on H563 (02/10): a use-after-free wrote plaintext over a block header.
 * memory.c refused the free ('Invalid Free'), then the cleanup at task exit
 * walked the chain, followed the ASCII 'next' of that header and took a
 * UsageFault UNALIGNED. Here the same: two blocks, the header of the second
 * overwritten with text, free it, return. The exit cleanup is the walk under
 * test. The second block sits BELOW the first (next-fit splits from the top of
 * the free block), so the walk meets the corrupted header before the good one.
 * ========================================================================== */
#define EX68_MEM_HDR_WORDS  7U     /* sizeof(caMemBlock) in memory.c = 28 B */

static u32 task_heap(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: user data over a heap block header, free it, exit");
    u8 *a = (u8 *)ca_memory_allocate(64);
    u8 *b = (u8 *)ca_memory_allocate(64);
    if (a == NULL || b == NULL) {
        ca_stdout_put_n("[EX68] heap: allocation failed");
        return 0;
    }
    /* "ABCD": odd low bits, so a 'next' taken from here is unaligned */
    volatile u32 *hdr = (volatile u32 *)(b - 4U * EX68_MEM_HDR_WORDS);
    for (u32 i = 0; i < EX68_MEM_HDR_WORDS; i++) hdr[i] = 0x44434241UL;
    bool freed = ca_memory_free(b, NULL, false);
    ca_stdout_printf_n("[EX68] heap: free of the corrupted block %s",
                       freed ? "ACCEPTED (wrong)" : "refused");
    ca_stdout_put_n("[EX68] heap: task exits, cleanup walks the chain");
    return 0;
}

/* Unlock of a mutex nobody owns. sync.c checks ownership AFTER
 * ca_spinlock_lock_irq(), so the safeguard fires with PRIMASK=1: the SVC
 * cannot be taken and escalates to HardFault FORCED with CFSR=0, which
 * hard_fault_recoverable() refuses -> the part RESETS. What is under test is
 * that the [SAFEGUARD] line gets out BEFORE the dump, naming the condition:
 * without it the dump shows only HFSR=FORCED, CFSR=0. */
static u32 task_sg_irq(void *arg) {
    (void)arg;
    ca_stdout_put_n("[EX68] provoking: kernel safeguard under PRIMASK (HardFault FORCED, then reset)");
    ca_os_sleep(100);
    ca_mutex_init(&s_sg_mtx);                 /* unlocked, owned by nobody */
    ca_mutex_unlock(&s_sg_mtx);
    return 0;
}

#if EN_TIMER
/* ============================================================================
 * FAULT INSIDE AN ISR — fault_in_handler_mode() in os_handlers.c
 *
 * Every case above faults in THREAD mode, where a task is to blame. Here the
 * divide by zero runs in the TIM3 update callback, i.e. in handler mode with
 * the frame on the MSP: there is no task to blame. The kernel must say
 * 'Faulted in handler mode' and HALT - not remove whatever task was
 * interrupted and return to thread mode with the ISR still active (H563 ex87,
 * 29/09: silent lock-up on M33). CLI only: the part stays halted.
 * ========================================================================== */
#define EX68_IRQ_TIM   3U          /* TIM3: present on every family */

static void irq_div0_cb(void) {
    /* 'forced' turns USGFAULTENA off and nobody turns it back on: without it
     * the div0 escalates to HardFault and this case would test that path. */
    SCB->SHCSR |= SCB_SHCSR_USGFAULTENA_Msk;
    SCB->CCR   |= SCB_CCR_DIV_0_TRP_Msk;
    __DSB(); __ISB();
    volatile u32 a = 100, b = 0;
    g_sink += a / b;                          /* UsageFault DIVBYZERO in handler mode */
}

static void fault_irq(void) {
    CLI_PUT_N("[EX68] provoking: divide by zero inside the TIM3 ISR (system halts)");
    ca_os_sleep(100);                         /* let the line out first */
    if (!ca_timer_init_periodic(EX68_IRQ_TIM, 1000U, irq_div0_cb)) {
        CLI_PUT_N("[EX68] irq: TIM3 init failed");
        return;
    }
    ca_timer_start(EX68_IRQ_TIM);
}
#endif

/* ============================================================================
 * CLI — chain of responsibility
 * ========================================================================== */

/* CA_FAULT_ASSERT = remove the task on fault (do NOT restart): a deterministic
 * fault (undef instr, div0, bad pointer) would just re-fault on restart, so the
 * task is killed and the CPU recovers. */
static void spawn(const char *name, ca_task_fn_t fn) {
    ca_os_job(name, CA_TH_LEVEL3, fn, NULL, STACK_SMALL, CA_JOB_STD, CA_FAULT_ASSERT);
}

static bool user_cli_handler(c_str *pcmd);
static ca_cli_node_t s_user_cli_node = { user_cli_handler, NULL };

static bool user_cli_handler(c_str *pcmd) {
    c_str cmd = *pcmd;
    const char *p = ca_skip_sp(cmd->data);

    if (ca_strncmp(p, "help", 4) == 0) {
        /* Two lines: one did not fit ca_stdout_put_n's buffer (DEFAULT_MSG_SIZE)
         * and came out cut at 'und~OVF' */
        CLI_PUT_N("ex68 fault: null_r | null_w | ram | flash | ibuserr | ro | stack | stackjmp | stackup | xn | runaway");
        CLI_PUT_N("ex68 fault: div0 | unalign | undef | invstate | forced" EX68_FP_HELP);
        CLI_PUT_N("ex68 fault: safeguard | sg_irq (kernel assertions, sg_irq resets the part)");
        CLI_PUT_N("ex68 fault: heap (corrupted block header: refused, no fault; heap stays corrupted)");
        CLI_PUT_N("ex68 fault: restart (div0 once in a RESTART task: renewed, runs again)");
#if EN_TIMER
        CLI_PUT_N("ex68 fault: irq (div0 inside the TIM3 ISR, halts the part)");
#endif
        return false;   /* continue the help chain */
    }

    if (ca_strncmp(p, "fault", 5) != 0) return false;
    p = ca_skip_sp(p + 5);

    if (ca_strncmp(p, "stackjmp", 8)== 0) { spawn("f_stackjmp",task_stackjmp);return true; }
    if (ca_strncmp(p, "stackup", 7) == 0) { spawn("f_stackup", task_stackup); return true; }
    if (ca_strncmp(p, "stack", 5)   == 0) { spawn("f_stack",   task_stack);   return true; }
    if (ca_strncmp(p, "null_r", 6)  == 0) { spawn("f_null_r",  task_null_r);  return true; }
    if (ca_strncmp(p, "null_w", 6)  == 0) { spawn("f_null_w",  task_null_w);  return true; }
    if (ca_strncmp(p, "ram", 3)     == 0) { spawn("f_ram",     task_ram);     return true; }
    if (ca_strncmp(p, "flash", 5)   == 0) { spawn("f_flash",   task_flash);   return true; }
    if (ca_strncmp(p, "runaway", 7) == 0) { spawn("f_runaway", task_runaway); return true; }
    if (ca_strncmp(p, "xn", 2)      == 0) { spawn("f_xn",      task_xn);      return true; }
    if (ca_strncmp(p, "ro", 2)      == 0) { spawn("f_ro",      task_ro);      return true; }
    if (ca_strncmp(p, "div0", 4)    == 0) { spawn("f_div0",    task_div0);    return true; }
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    if (ca_strncmp(p, "fpctx", 5)   == 0) { spawn("f_fpctx",   task_fpctx);   return true; }
    if (ca_strncmp(p, "fp", 2)      == 0) { spawn("f_fp",      task_fp);      return true; }
#endif
    if (ca_strncmp(p, "unalign", 7) == 0) { spawn("f_unalign", task_unalign); return true; }
    if (ca_strncmp(p, "undef", 5)   == 0) { spawn("f_undef",   task_undef);   return true; }
    if (ca_strncmp(p, "ibuserr", 7) == 0) { spawn("f_ibuserr", task_ibuserr); return true; }
    if (ca_strncmp(p, "invstate", 8)== 0) { spawn("f_invstate",task_invstate);return true; }
    if (ca_strncmp(p, "forced", 6)  == 0) { spawn("f_forced",  task_forced);  return true; }
    if (ca_strncmp(p, "safeguard", 9)==0) { spawn("f_safeguard",task_safeguard);return true; }
    if (ca_strncmp(p, "sg_irq", 6)  == 0) { spawn("f_sg_irq",  task_sg_irq);  return true; }
    if (ca_strncmp(p, "heap", 4)    == 0) { spawn("f_heap",    task_heap);    return true; }
    if (ca_strncmp(p, "restart", 7) == 0) {
        /* The one case that must NOT be CA_FAULT_ASSERT: it tests the renew. */
        g_restart_runs = 0U;
        ca_os_job("f_restart", CA_TH_LEVEL3, task_restart, NULL, STACK_SMALL, CA_JOB_STD,
                  CA_FAULT_RESTART_THREAD);
        return true;
    }
#if EN_TIMER
    if (ca_strncmp(p, "irq", 3)     == 0) { fault_irq();                      return true; }
#endif

    CLI_PUT_N("ex68: unknown fault (type 'help')");
    return false;
}

/* ============================================================================
 * MONITOR + ENTRY POINTS
 * ========================================================================== */

/* Automatic sequence: run each fault in order of increasing severity, one at a
 * time, with a gap so the handler can print and the system can recover before
 * the next */
#define FAULT_GAP_MS   2000

typedef struct { const char *name; ca_task_fn_t fn; } fault_entry_t;

/* All faults are handled and recover (BUSFAULTENA|USGFAULTENA|DIV_0_TRP enabled
 * in ca_bus_error_init). Grouped by fault type for readability. */
static const fault_entry_t s_seq[] = {
    /* --- BusFault (data) --- */
    { "null_r",  task_null_r  },   /* often no fault (0x0 readable on some parts) */
    { "null_w",  task_null_w  },   /* write 0x0    -> BusFault/MemManage */
    { "ram",     task_ram     },   /* beyond RAM   -> BusFault           */
    { "flash",   task_flash   },   /* beyond flash -> BusFault           */
    { "ibuserr", task_ibuserr },   /* fetch past flash -> BusFault IBUSERR */
    /* --- MemManage (MPU) --- */
    { "ro",      task_ro      },   /* write RO canary  -> MemManage DACCVIOL */
    { "stack",   task_stack   },   /* write into 32B canary band -> MemManage DACCVIOL */
    { "stackjmp",task_stackjmp},   /* jump OVER band (256B below)  -> MemManage DACCVIOL */
    { "cross",   task_cross   },   /* write into ANOTHER task's stack -> DACCVIOL, must name the victim */
    { "xn",      task_xn      },   /* exec from data UNDER an XN region -> MemManage IACCVIOL */
    { "runaway", task_runaway },   /* exec from data with NO region -> runs on: UsageFault, varies */
    /* --- PSP range-check (software, not a CPU fault) --- */
    { "stackup", task_stackup },   /* corrupt another task's saved PSP upward -> killed at incoming-task check */
    /* --- UsageFault --- */
    { "div0",    task_div0    },   /* /0            -> UsageFault DIVBYZERO */
    { "unalign", task_unalign },   /* odd addr u32  -> UsageFault UNALIGNED */
    { "undef",   task_undef   },   /* UDF.W         -> UsageFault UNDEFINSTR */
    { "invstate",task_invstate},   /* non-Thumb branch -> UsageFault INVSTATE */
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    /* --- FP context (the only cases that produce an EXTENDED frame) --- */
    { "fp",      task_fp      },   /* BusFault with FP armed -> EXC_RETURN must read ...AC */
    { "fpctx",   task_fpctx   },   /* callee-saved + s16 across context switches (no fault) */
#endif
    /* --- Escalated HardFault (HFSR.FORCED, downgraded to per-task) --- */
    { "forced",  task_forced  },   /* USGFAULTENA off + div0 -> HardFault FORCED, recovered per-task */
};
#define SEQ_LEN  (sizeof(s_seq) / sizeof(s_seq[0]))

static u32 task_monitor(void *arg) {
    (void)arg;
    ca_os_sleep(2000);
    ca_stdout_put_n("=== Example 68: Fault sequence (auto start) ===");

    for (u32 i = 0; i < SEQ_LEN; i++) {
        ca_stdout_printf_n("\r\n--- [%u/%u] fault: %s ---", i + 1, (u32)SEQ_LEN, s_seq[i].name);
        ca_os_sleep(200);                 /* let the header flush first */
        spawn(s_seq[i].name, s_seq[i].fn);
        ca_os_sleep(FAULT_GAP_MS);        /* fault fires, handler prints, recover */
    }

    ca_stdout_put_n("\r\n=== Fault sequence complete - system still alive ===");
    while (1) {
        ca_os_sleep(10000);
        ca_stdout_printf_n("EX68 alive  tick=%u", (unsigned)ca_os_tick_ms());
    }
    return 0;
}

void ca_os_configure(void) {
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
    EVAL_USB_CDC_STD(ca_usb_dev_mng_cdc_cli();)

#if __MPU_PRESENT
    /* Armed ONCE, here, and never taken down.
     *
     * Arming it inside the case would mean tearing it down afterwards, and the
     * case never returns - the fault removes the task. Leaving MPU state to be
     * cleaned up by whoever runs next is how a bench starts depending on the
     * order its cases run in. As a property of the example instead, the two
     * buffers differ in exactly one way for the whole life of the firmware:
     * g_xn_buf is execute-never, g_data_buf is not.
     *
     * THE LAST REGION, EXPLICITLY - not ca_mpu_add_region().
     *
     * The low regions are PRE-ASSIGNED: 0 is the MSP canary, 1 and 2 the PSP
     * stack guard of the running task (mpu.h:166-169), and they are
     * reprogrammed at every context switch. ca_mpu_add_region() takes
     * ca_mpu_first_free_region(), which scans from 0 and only looks at
     * RASR.ENABLE - and here, before ca_os_run(), the scheduler has not armed
     * the canaries yet, so region 1 reads as free. MEASURED 18/09: it handed
     * out region 1, the first context switch took it back ('mpu' showed R1 on
     * a stack canary), and this case then measured exactly what it measured
     * before the fix - a runaway, UNDEFINSTR at the same PC as its twin.
     *
     * The highest index is also the right one on ARMv7-M for a second reason:
     * where regions overlap, the highest-numbered one wins, so nothing the
     * kernel arms later can override this one.
     *
     * The result is kept: negative means it could not be armed, and the case
     * has to say so rather than quietly become the runaway one. */
    const u32 total = ca_mpu_get_total_regions();
    if (total == 0U ||
        ca_mpu_update_region((u16)(total - 1U), (u32)g_xn_buf,
                             sizeof(g_xn_buf), MPU_ACCESS_RDWR) < 0) {
        g_xn_region = -1;
    } else {
        g_xn_region = (int)(total - 1U);
    }
#endif
}

void ca_os_main(void) {
#if EN_TIMER
    ca_timer_init();
#endif
    ca_os_job("monitor", CA_TH_LEVEL3, task_monitor, NULL, STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    EVAL_CLI( ca_cli_register(&s_user_cli_node); )
    ca_os_run(true);
}
