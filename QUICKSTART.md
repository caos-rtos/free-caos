# caOS FREE — quick start

## 1. Prerequisites

Building and flashing use two different sets of tools. You need the first to build the
firmware. You need the second only if you have a board; without one, see
[No board?](#no-board-run-it-on-renode) below.

### Build: compiler, CMake, make

`./bootstrap` and `./do` take `arm-none-eabi-gcc` from your `PATH`, and they also need
**CMake** ≥ 3.20 and **make**. Use any one of these compilers:

- **From your distribution** (Ubuntu/Debian). The package is `gcc-arm-none-eabi`; it
  pulls in the C library `libnewlib-arm-none-eabi`:

  ```bash
  sudo apt install gcc-arm-none-eabi cmake make
  ```

  Ubuntu 22.04 ships GCC 10.3; the package builds and runs with it.
- **Arm GNU Toolchain** from
  [developer.arm.com](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  (`arm-none-eabi`, x86_64 Linux host). Unpack it and add its `bin/` to `PATH`.
- **The compiler bundled with STM32CubeIDE**, if you already have the IDE. The libraries
  of the package are built with it (GCC 13.3). It is **not** in the STM32CubeProgrammer
  folder: it sits inside the IDE plugins, in a folder whose name carries the version, e.g.

  ```bash
  export PATH=/opt/st/stm32cubeide_1.19.0/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.13.3.rel1.linux64_1.0.0.202410170706/tools/bin:$PATH
  ```

  Look for `*gnu-tools-for-stm32*/tools/bin` under your IDE's `plugins/`.

Check:

```bash
arm-none-eabi-gcc --version
cmake --version
```

### Flash a board: STM32CubeProgrammer

The on-board ST-LINK of the NUCLEO is the programmer; the flashing scripts
(`prog.sh`, the test bench) drive it through **`STM32_Programmer_CLI`**:

- Install **STM32CubeProgrammer** from st.com (a separate download from the IDE). Its
  default Linux folder is `/opt/st/STM32Cube/STM32CubeProgrammer`, with the command line
  tool in `bin/`.
- STM32CubeIDE also contains a copy, inside its plugins:
  `plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.linux64_<version>/tools/bin`.
- Set **`STM32_PRG_PATH`** to that `bin/` folder. The scripts take
  `STM32_Programmer_CLI` from there, and stop with an error if the variable is not set.
  Put it in your `~/.bashrc`, e.g.:

  ```bash
  export STM32_PRG_PATH=/opt/st/STM32Cube/STM32CubeProgrammer/bin
  ```

openocd works too, by hand: `prog.sh` uses only STM32CubeProgrammer.

### Bootloader only

For `workspace/app_2` you also need:
- **python3** ≥ 3.6 (standard library only);
- **OpenSSL** ≥ 1.1.1, to sign the images (`tools/caos_image.py`);
- **dfu-util**, for the USB download.

The TFTP client is `tools/caos_tftp`, in the package.

### No board? Run it on Renode

The [Renode](https://renode.io) simulator runs the same `.elf` files on a
simulated NUCLEO, for all seven boards of the FREE edition. The caOS console appears in
your terminal. You need only the build tools above and Renode, and no STM32CubeProgrammer.
Installation and use are in **`simu/QUICKSTART.md`**; in short:

```bash
./bootstrap -m H753 && ./do 2
simu/run.sh h753 elfs/H753/h753_example_2.elf 5000           # headless, 5 s simulated
simu/run.sh -i h753 elfs/H753/h753_example_68.elf            # interactive console
simu/run.sh -i -e f767 elfs/F767/f767_example_35.elf         # + Ethernet to your PC (TAP)
simu/run.sh -w f767 elfs/F767/f767_example_29.elf 3000 "help;wire"   # + bench jumpers
simu/test.sh                                                 # the automatic Renode tests
```

`-e` (Ethernet, on F439/F767/H753) needs root once to create the TAP. The simulation is
faithful for the kernel, the scheduler, the MPU, faults, timers and the console; the time
is not real, and USB and most acceleration peripherals are not modelled: the details are
in `simu/LIMITS.md`.

(Renode is a trademark of Antmicro; caOS is not affiliated with Antmicro and does not
ship Renode.)

## 2. Unpack

```bash
tar xjf free_caos-free-vX.Y.Z.tar.bz2     # the archive of the release you downloaded
cd free_caos
```

Releases: **https://github.com/caos-rtos/free-caos/releases**.

## 3. Choose the board and build

The seven boards: `F439 F446 F767 H563 H753 H773 U575` (H773 = NUCLEO-H7S3L8).

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

**NUCLEO-H7S3L8 (H773)**: the firmware is linked at `0x70000000` and runs from the
Octo-SPI flash on XSPI2; `prog.sh` writes it there through the external loader of
STM32CubeProgrammer. The internal flash holds only the trampoline that sets up XSPI2 and
jumps to it: flash it **once**, before anything else, with `./prog.sh -s -a 5`. Details,
recovery and the B2 button in `workspace/app_5/doc/app_5_user_guide_en.md`.

## 5. Write your own program

Your applications live in `workspace/app_N`, one directory and one firmware each, all
built with the same fixed `include/os_config.h` (there is no per-application config in
the FREE edition). Generate a new one with `new_app.py` — it asks how many tasks, their names and
levels — or copy the template:

```bash
./new_app.py              # asks, then writes workspace/app_<next free>/
./new_app.py -t sensor:4 -t logger:2:medium   # the same, without questions
./new_app.py -h           # the options of a task: stack, fpu, fault, period

cp -r workspace/app_1 workspace/app_6         # or by hand, to a FREE number:
# app_2, app_4, app_5 and app_50 are the reference applications of the package
# edit workspace/app_6/ca_os_app_1.c (rename it ca_os_app_6.c if you like)
./bootstrap -m H563 && ./do -a 6 && ./prog.sh -s -a 6
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

The full API is in `include/` and in the manual (`doc/`). To look up a function, a
define or a type without opening the headers:

```bash
tools/lookup_sym.py ca_os_job            # signature, header, parameters, return, notes, example
tools/lookup_sym.py --list ca_uart       # every symbol with a prefix
tools/lookup_sym.py --find queue timeout # symbols whose name or description has all the words
tools/lookup_qa.py ca_queue_send         # questions and answers: intent, common mistakes
tools/lookup_qa.py --type error ca_uart_put   # one kind: usage code parameters return error behavior hw_diff
```

The data is `tools/db/reference.json` (the reference chapter of the manual) and
`tools/db/caos_qa.jsonl` (Q&A generated with the help of an AI from that reference). When a
detail matters, the declaration in `include/` is the authority. Some entries document CAN, USB HS
and MicroPython, which the FREE configuration leaves off: if a name is not declared in `include/`, this
edition does not have it.

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

- `-m <MCU>` is the board of the run (`F439 F446 F767 H563 H753 H773 U575`); the elfs for it
  must already be in `elfs/<MCU>/`.
- `-n ex<NN>` selects one example's test class; without it the whole suite runs
  (every class whose firmware is present).
- The console is found automatically from the ST-LINK; set `CAOS_TTY=/dev/ttyACMn`
  if more than one board is connected.

### Every board in one go: `eol.sh`

`./eol.sh` is the end-of-line run: the whole bench on each board, one board at a
time. For every board it builds the firmware (`./bootstrap -m <MCU>` and `./do`),
asks you to connect that Nucleo, waits until its ST-LINK shows up (if it does not,
it tells you to check the cable and looks again), runs the full suite and writes
the report to `end_of_line/<mcu>_self_test.html` (plus `.xml` and the complete
`.log`). Then it asks for the next board. A board that is not the one asked for is
recognised from its Device ID and refused before anything runs.

```bash
./eol.sh                  # the seven boards, in order
./eol.sh H753 U575        # only these
```

Run it as your normal user: it asks for the `sudo` password once, for the network
classes. On the NUCLEO-H7S3L8 it flashes the trampoline (`workspace/app_5`) first.
At every prompt, Enter goes on, `s` skips the board, `q` stops.

Some classes need extra hardware or a dedicated test network (a QSPI flash module,
a point-to-point Ethernet link): those are the examples flagged in **EXAMPLES.md**
and in their `fail_exNN_info.md`.

The reports of the run made before each release, on all seven boards, are published in
the repository (`end_of_line/`): compare yours with them.

## Notes

- The configuration (`include/os_config.h`) is **fixed**: it is the one the libraries were
  compiled with. Changing it does not change the precompiled libraries.
- Some examples do not run on the FREE edition; those carry a `fail_exNN_info.md` next to
  their source explaining why, and `./do` skips them. See **EXAMPLES.md**.
- Read **LIMITS.md** for what the FREE edition does and does not provide.
- The keys shipped with the bootloader (`workspace/app_2`) and the SSH examples are **demo
  keys**, public by definition: replace them with your own before any use outside the bench
  (see the manual).
