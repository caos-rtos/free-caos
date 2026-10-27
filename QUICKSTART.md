# caOS FREE — quick start

## 1. Prerequisites

- **Arm GNU Toolchain** (`arm-none-eabi-gcc`) and **CMake** ≥ 3.20 and **make**, on your `PATH`.
- A programmer for your board (the on-board ST-LINK is enough) and its flashing tool
  (STM32CubeProgrammer, or openocd).
- **`STM32_PRG_PATH`** set to the `bin/` directory of STM32CubeProgrammer — the flashing
  scripts take `STM32_Programmer_CLI` from there and stop with an error when it is not set.
  Put it in your `~/.bashrc`, e.g.:

  ```bash
  export STM32_PRG_PATH=/opt/st/STM32Cube/STM32CubeProgrammer/bin
  ```
- Only for the bootloader (`workspace/app_2`): **python3** ≥ 3.6 (standard library only),
  **OpenSSL** ≥ 1.1.1 to sign the images (`tools/caos_image.py`), **dfu-util** for the
  USB download. The TFTP client is `tools/caos_tftp`, in the package.

Check the toolchain:

```bash
arm-none-eabi-gcc --version
cmake --version
```

## 2. Unpack

```bash
tar xjf free_caos-free-v0.1.0.tar.bz2
cd free_caos
```

## 3. Choose the board and build

The five boards: `F446 F767 H563 H753 U575`.

```bash
./bootstrap -m H563      # configures for the NUCLEO-H563ZI (writes cpu.cmake)
./do                     # builds every example that works on the FREE edition
```

The firmware lands in `elfs/<MCU>/`, e.g. `elfs/H563/h563_example_1.elf`.

Build only some examples, or a project of yours:

```bash
./do 1 33                 # examples 1 and 33 only
./do -a 1                 # your application in workspace/app_1
./clean                   # remove build/ and elfs/
```

## 4. Flash

With the on-board ST-LINK, through `prog.sh` (it reads the board from `cpu.cmake`):

```bash
./prog.sh -s -e 1                         # example 1 -> elfs/<MCU>/<mcu>_example_1.elf
./prog.sh -s -a 1                         # your application workspace/app_1
./prog.sh -s -f elfs/H563/h563_example_1.elf # any .elf, by path
./prog.sh -h                              # all the options (-j for a J-Link)
```

`prog.sh` runs `$STM32_PRG_PATH/STM32_Programmer_CLI` (see Prerequisites). With two or
more ST-LINK connected it picks the one whose board carries the MCU of the `.elf`. You can also load the `.elf` from your IDE or openocd.

## 5. Write your own program

Your applications live in `workspace/app_N`, one directory and one firmware each, all
built with the same fixed `include/os_config.h` (there is no per-application config in
the FREE edition). Generate a new one with `new_app.py` — it asks how many tasks, their names and
levels — or copy the template:

```bash
./new_app.py              # asks, then writes workspace/app_<next free>/
./new_app.py -t sensor:4 -t logger:2:medium   # the same, without questions
./new_app.py -h           # the options of a task: stack, fpu, fault, period

cp -r workspace/app_1 workspace/app_5         # or by hand, to a FREE number:
# app_2, app_3, app_4 and app_50 are the reference applications of the package
# edit workspace/app_5/ca_os_app_1.c (rename it ca_os_app_5.c if you like)
./bootstrap -m H563 && ./do -a 5 && ./prog.sh -s -a 5
```

`bootstrap -m` writes the board into `cpu.cmake`, and both `./do` and `prog.sh -a` read
it from there: the firmware you build and the one you flash are for the same board.
`new_app.py` prints this line with the MCU of `cpu.cmake` (or the one given with `-m`).

A caOS program provides two entry points:

```c
#include "caos.h"

void ca_os_configure(void) { /* set up drivers/services before the scheduler */ }

void ca_os_main(void) {
    ca_os_job("hello", CA_TH_LEVEL4, my_task, NULL,
              STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
    ca_os_run(true);      // start the scheduler — does not return
}
```

The full API is in `include/` and in the manual (`doc/`).

## 6. Debugging without a debugger

The libraries are Release builds without sources, so stepping into them would show
bare assembly anyway, and the FREE firmware refuses a halting debugger (LIMITS.md).
What you debug is **your** code, and caOS gives you two ways to look inside it while
it runs, both through the console.

**Read a variable by name, from outside.** The `.elf` built by `./do` keeps the
symbol table of your code: `peek.py` finds where a global or `static` variable lives
and reads it with the CLI commands `mem` / `hexdump`, from inside the board.

```bash
./peek.py elfs/H563/h563_app_1.elf --list s_         # your variables starting with s_
./peek.py elfs/H563/h563_app_1.elf s_count           # the CLI command to type
./peek.py elfs/H563/h563_app_1.elf s_count -p /dev/ttyACM0             # send it, decode
./peek.py elfs/H563/h563_app_1.elf s_count -p /dev/ttyACM0 --set 2A    # write 0x2A
```

On the console itself: `mem 20000060` reads a 32-bit word, `mem 20000060 2A` writes
it (hex, no `0x`, 4-byte aligned), `hexdump 20000060 2` dumps 2 lines of 16 bytes.
Writing is how you try a fix live, before rebuilding. Only variables with a fixed
address can be read this way: globals and statics, not locals in a register or on a
task stack. Close any other terminal on the port first.

**Watch a variable from inside, with a task.** caOS is multitasking, so the most
natural probe is one more task that prints what you care about when it changes:

```c
static volatile u32 s_count;          /* written by your code */

static u32 task_watch(void *arg)
{
    (void)arg;
    u32 last = s_count;
    for (;;) {
        u32 now = s_count;
        if (now != last) {
            ca_stdout_printf_n("watch: s_count %u -> %u", last, now);
            last = now;
        }
        ca_os_sleep(50U);             /* poll period: what it can miss is shorter */
    }
    return 0;
}

/* in ca_os_main(), before ca_os_run(): */
ca_os_job("watch", CA_TH_LEVEL1, task_watch, NULL,
          STACK_SMALL, CA_JOB_STD, CA_FAULT_RESTART_THREAD);
```

At the lowest levels it costs almost nothing, and it can stay in the code behind a
`#define`. Alongside: `top` (CPU per task), `taskall` (state and stack of every task),
`free` (heap), `canary` (stack guards), `memmap` (memory layout).

## 7. Run the test bench (optional)

The package ships the same bench used to validate caOS: `test/hw_test`, a Linux
host program that drives the board over its ST-LINK — it flashes an example from
`elfs/<MCU>/`, talks to its console and checks the result. It does **not** build
anything, so build the firmware first with `./do`. The wrapper `test/run_test.sh`
runs it with the FREE edition already selected.

Prerequisites: Linux, the board's ST-LINK connected, and **STM32CubeProgrammer**
(`STM32_Programmer_CLI`) installed — the bench calls `./prog.sh` to flash.

**Run the bench with `sudo`.** The network classes configure the bench link
themselves — the PC address `192.168.10.1`, the DHCP server, the PC-side peers —
through `./tools/bench_net.sh`, and that needs root. Build with `./do` as your
normal user; only the bench runs as root. `sudo` clears the environment, so pass
`STM32_PRG_PATH` through with `--preserve-env`, as below. The network classes also need
`iproute2`, `dnsmasq`, `ethtool`, `tcpdump`, `iperf`, `socat`, `python3` and
NetworkManager (`nmcli`).

```bash
./do 68                                   # build the example(s), as normal user
S="sudo --preserve-env=STM32_PRG_PATH"
$S ./test/run_test.sh -m U575 -n ex68     # flash ex68 on the U575 and check it
$S ./test/run_test.sh -m H563 -l          # list the test classes, run nothing
$S ./test/run_test.sh -m F767 -n ex84 -t 3   # a single test of a class
```

> `bench_net.sh` reconfigures the Ethernet port wired to the board. Use a
> dedicated port with a point-to-point cable to the NUCLEO, never the port of
> your office or home LAN.

- `-m <MCU>` is the board of the run (`F446 F767 H563 H753 U575`); the elfs for it
  must already be in `elfs/<MCU>/`.
- `-n ex<NN>` selects one example's test class; without it the whole suite runs
  (every class whose firmware is present).
- The console is found automatically from the ST-LINK; set `CAOS_TTY=/dev/ttyACMn`
  if more than one board is connected.

Some classes need extra hardware or a dedicated test network (a QSPI flash module,
a point-to-point Ethernet link): those are the examples flagged in **EXAMPLES.md**
and in their `fail_exNN_info.md`.

## Notes

- The configuration (`include/os_config.h`) is **fixed**: it is the one the libraries were
  compiled with. Changing it does not change the precompiled libraries.
- Some examples do not run on the FREE edition; those carry a `fail_exNN_info.md` next to
  their source explaining why, and `./do` skips them. See **EXAMPLES.md**.
- Read **LIMITS.md** for what the FREE edition does and does not provide.
- The keys shipped with the bootloader (`workspace/app_2`) and the SSH examples are **demo
  keys**, public by definition: replace them with your own before any use outside the bench
  (see the manual).
