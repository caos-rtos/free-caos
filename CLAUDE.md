# CLAUDE.md — working with the caOS FREE package

Guidance for an AI coding assistant (Claude Code or similar) helping a user write firmware
on top of this package. It applies to the FREE edition: precompiled libraries, public
headers, examples and manuals — **no kernel or driver sources**.

## What is in the package

| Path | Content |
|---|---|
| `include/` | Public headers. `caos.h` includes all of them. This is the API reference. |
| `include/os_config.h` | The configuration the libraries were built with. **Fixed**: do not edit. |
| `<fam>/<mcu>/` | `libcaOS.a`, `libcaOS_<MCU>.a`, linker script, `objmap.h` (board map: LEDs, UARTs, stack sizes, tick) |
| `examples/example_N/` | `ca_os_exN.c` — one feature per example, plus PC-side test scripts |
| `workspace/app_N/` | The user's applications, one firmware each; `app_1` is the template. `app_2` (bootloader: TFTP/DFU update, A/B slots, rollback), `app_3`, `app_4` … are complete reference applications with their guides and PC scripts |
| `doc/` | `caos_manual_en.pdf`, `caos_manual_it.pdf` |
| `README.md` `QUICKSTART.md` `EXAMPLES.md` `LIMITS.md` | Start here; `LIMITS.md` says what the FREE edition leaves out |

Supported boards: NUCLEO-F446ZE, -F767ZI, -H563ZI, -H753ZI, -U575ZI-Q.

## Build and run

```bash
./bootstrap -m H563     # pick the board (F446 F767 H563 H753 U575), writes cpu.cmake
./do                    # all examples      -> elfs/<MCU>/<mcu>_example_N.elf
./do 1 33               # some examples
./new_app.py -t sensor:4 -t logger:2   # new workspace/app_N skeleton (no -t: asks)
./do -a 1               # an application in workspace/app_1
./prog.sh -s -e 1       # flash example 1 (-a N: app_N, -f FILE: any .elf)
./clean
```

The user builds and flashes. When something fails, ask for the compiler output or the
console log rather than guessing.

## Finding an API

There is no symbol-lookup tool in the package: search `include/` (and the examples, which
are the best usage reference). **Do not invent function names by convention** — the real
names often differ from the "obvious" one (`ca_os_sleep`, not `ca_task_delay`;
`ca_memory_allocate`, not `ca_memory_alloc`). If a function is not in `include/`, it does
not exist in this edition: say so instead of guessing.

## Code conventions

- Every `.c` includes only `#include "caos.h"`.
- Types: `u8 u16 u32 i8 i16 i32`, `bool`; strings are `c_str` (a pointer to a caOS string).
- Bare metal on CMSIS, **no ST HAL**. caOS owns the clock and the SysTick: do not add
  `SystemClock_Config()` or `HAL_InitTick()`.
- Console output is in English and ASCII.

## Program structure

```c
#include "caos.h"

static u32 worker(void *arg)          /* every task: u32 f(void *arg) */
{
    (void)arg;
    for (;;) {
        /* ... */
        ca_os_sleep(100);             /* ms */
    }
    return 0;                          /* returning ends the task cleanly */
}

void ca_os_configure(void) { /* drivers and services, before the scheduler */ }

void ca_os_main(void)
{
    ca_os_job("worker", CA_TH_LEVEL3, worker, NULL,
              STACK_MEDIUM, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);                   /* starts the scheduler, does not return */
}
```

- `ca_os_job()` makes the task runnable at once; `ca_os_job_suspend()` creates it suspended
  (release it with `ca_os_resume_handle()`).
- Keep the returned `ca_task_handle_t`. Never store the numeric `thid`: it is a position in
  the task list and changes when another task is removed. A stale handle fails safely.
- Tasks that use floating point must be created with an FPU job type, not `CA_JOB_STD`.

## Semantics that differ from other RTOSes

- **Priorities are CFS weights**, `CA_TH_LEVEL0..6` = 1, 2, 4 … 64. A higher level gets a
  larger CPU share; it does **not** starve the lower ones. Never use priority as mutual
  exclusion: use a mutex, or `ca_os_sched_lock()` / `ca_os_sched_unlock()` (never block
  while holding it).
- **Timeouts are in milliseconds**, on every blocking call: `CA_NO_WAIT_TIME` (0) = do not
  wait, `CA_WAIT_FOREVER` = wait forever.
- **Wake-up order on IPC is FIFO**, not by priority.
- **An ISR does not preempt into the woken task**: the switch happens at the next tick
  (100-250 µs), or at once if the ISR interrupted the idle task. Put deadline-critical work
  in the ISR itself.
- A blocked wait cannot be aborted from outside: use a timeout and recheck a condition.
- `ca_os_delay_ms()` is a busy wait for **before** the scheduler; from a task use
  `ca_os_sleep()` / `ca_os_sleep_until()`.

## Memory

- `STACK_SMALL/MEDIUM/LARGE/KERNEL` are **ratios of `STACK_MIN`**, which depends on the board
  (`objmap.h`): 512 B on F446, 1 KB on F767, 2 KB on H563/H753/U575. The same `STACK_SMALL`
  is 1 KB on an F446 and 4 KB on a U575: size on the smallest target. Stacks are always
  allocated by the kernel, with MPU guard and canary; there is no user-supplied stack.
- `ca_memory_allocate()` blocks **belong to the calling task** and are freed when it ends.
  A block handed to another task (message, buffer, shared object) must come from
  `ca_memory_allocate_shared()` and be freed with `ca_memory_free()`.
- **CLI commands run in a temporary task** that ends when the command returns: anything a
  command allocates with `ca_memory_allocate()` is freed right after. Keep long-lived
  resources in a permanent task, or allocate them shared.
- The FREE edition reserves half of the heap (`LIMITS.md`).

## Console and CLI

- `ca_stdout_put_n()` / `ca_stdout_printf_n()` work **only from a task**. Before
  `ca_os_run()` (in `ca_os_configure()`) print with `PUT_N()` on a `PRIVATE_STRING`.
- A user CLI command is a node in a chain:

  ```c
  static bool my_cli(c_str *pcmd)
  {
      const char *p = ca_skip_sp((*pcmd)->data);
      if (ca_strncmp(p, "help", 4) == 0) { CLI_PUT_N("start | stop"); return false; }
      if (ca_strncmp(p, "start", 5) == 0) { CLI_PUT_N("started"); return true; }
      return false;                     /* not mine: pass it on */
  }
  static ca_cli_node_t s_my_node = { my_cli, NULL };
  /* in ca_os_configure(), after ca_cli_init(): ca_cli_register(&s_my_node); */
  ```

  Return `true` when handled, `false` to pass it to the next node; on `help` print one line
  and return `false` so the other nodes add theirs.
- An `EN_CLI_x` flag only compiles a driver's commands; they appear when that driver's
  `_init()` runs.

## Drivers: traps worth knowing

- **Pins are board facts.** The same signal sits on different pins/connectors on each
  Nucleo; the examples carry per-board `#if MCU_ID == ...` blocks. LEDs come from `objmap.h`
  through `ca_led_*()` — use those rather than hard-coded GPIOs.
- `ca_uart_*` numbers start at 1: `uart_num == 0` is rejected. LPUART has its own API
  (`lpuart.h`).
- PWM: the period is **ARR+1** timer ticks and duty is in permille; ARR is derived from the
  timer clock, which differs per MCU.
- Debugging the FREE firmware with a halting debugger is not supported (`LIMITS.md`): debug
  from inside, through the console. To read the user's variables, take the address from
  the `.elf` and ask the board: `./peek.py <elf> <name> -p <port>` (globals and statics;
  `--set HEX` writes a 32-bit word), or the CLI directly (`mem <addr> [value]`,
  `hexdump <addr> <lines>`, hex without `0x`). To follow a value over time, a low-level
  task that prints it when it changes. Also `top`, `taskall`, `free`, `canary`, `mpu`,
  `memmap` and the safeguard messages. Read the board before guessing from the code.

## Faults

Each task has its own fault policy (the last argument of `ca_os_job()`):
`CA_FAULT_RESTART_THREAD` restarts it, `CA_FAULT_ASSERT` removes it, the emergency variants
call a handler first. A fault in one task does not stop the others. `CA_SAFEGUARD(cond, msg)`
is the assert; it uses the same mechanism.

## Migrating from FreeRTOS, Zephyr or ThreadX

Chapter "Migrating from FreeRTOS, Zephyr and ThreadX to caOS" in the manual: call-by-call
tables and the semantic differences above, in detail.
