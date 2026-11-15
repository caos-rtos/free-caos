# caOS on Renode — quick start (no hardware needed)

No NUCLEO board on your desk? You can still build the caOS examples and watch them run:
[Renode](https://renode.io) simulates the board, and the caOS console appears in your
terminal. The `.elf` is the same file you would flash on the real board.

What you get: the kernel, the scheduler, tasks, fault handling and the console on a
simulated STM32. What you do not get: real timing (see
[Limits](#limits)), Ethernet, USB, or most of the peripherals the examples drive.

> **Renode** is a trademark of Antmicro. caOS is not affiliated with, endorsed or
> sponsored by Antmicro. Renode is free software under the MIT license, developed
> by Antmicro and distributed by them. caOS does **not** ship any Renode file: the
> scripts in this folder use your own Renode installation.

## 1. What you need

| Tool | What for | Where it comes from |
|---|---|---|
| `arm-none-eabi-gcc`, `cmake` ≥ 3.20, `make` | build the firmware | see the package `QUICKSTART.md`, section 1 |
| Renode ≥ 1.17 (`renode`, `renode-test`) | run the firmware | section 2 below |
| `python3` ≥ 3.8 with `venv` | the automatic tests (`renode-test`) | your distribution |

Linux only: the scripts are bash. We test on Ubuntu 22.04, x86-64.

On Ubuntu/Debian the compiler can come straight from the distribution:

```bash
sudo apt install gcc-arm-none-eabi cmake make python3-venv
```

## 2. Install Renode

These are the paths described in Renode's own README
([github.com/renode/renode](https://github.com/renode/renode#installation)). Pick **one**.

### a) Linux portable package (recommended: includes everything)

It includes the .NET runtime, so there is nothing else to install. It needs GTK2 only
for the graphical windows, which these scripts do not use.

```bash
mkdir -p ~/renode_portable && cd ~/renode_portable
wget https://builds.renode.io/renode-latest.linux-portable.tar.gz
tar xf renode-latest.linux-portable.tar.gz --strip-components=1
export PATH="$HOME/renode_portable:$PATH"     # put this line in ~/.bashrc
```

Prefer a numbered release to `latest`? Take the `linux-portable` archive from
[github.com/renode/renode/releases](https://github.com/renode/renode/releases).

### b) Debian/Ubuntu package

Download the `.deb` from the [releases page](https://github.com/renode/renode/releases)
(or `https://builds.renode.io/renode-latest.deb`) and install it:

```bash
sudo apt install ./renode_*.deb
```

It installs into `/opt/renode` with `renode` and `renode-test` in `/usr/bin`. This
package **needs the .NET runtime ≥ 6.0** on the system: if `renode` prints
`dotnet not found`, install it by following
[Microsoft's guide](https://learn.microsoft.com/en-us/dotnet/core/install/linux).

### Check

```bash
renode --version          # Renode v1.17.0 or later
```

The scripts find the Renode folder by themselves: the folder of `renode` (portable) or
`/opt/renode` (package). If yours is somewhere else, set `export RENODE_ROOT=/path/to/renode`.

### Python modules for `renode-test` (only for the automatic tests)

`renode-test` runs Robot Framework suites and needs a few Python modules. Keep them in a
virtual environment inside `simu/`, where `test.sh` finds it:

```bash
python3 -m venv simu/.venv
simu/.venv/bin/pip install -r /opt/renode/tests/requirements.txt
#                             ~/renode_portable/tests/requirements.txt for the portable package
```

## 3. Build the examples

The simulation runs the same `elfs/<MCU>/*.elf` that `./do` builds for the board:

```bash
./bootstrap -m H753       # one of the boards in section 5
./do 1 2                  # examples 1 and 2 -> elfs/H753/h753_example_{1,2}.elf
```

## 4. Run one example

```bash
simu/run.sh h753 elfs/H753/h753_example_2.elf 5000
```

Arguments: the board, the `.elf`, and how long to simulate in **milliseconds of
simulated time** (default 500). This mode is **headless**: no window opens, and the
console output is printed when the simulation ends (about 20 s for 5000 ms).
`simu/run.sh --help` lists both modes: this one and the interactive one (`-i`),
described below. For example 2 you should see the caOS banner and two tasks greeting each other:

```
TASK 2: par1=3
TASK 2: hello!
TASK 1: par1=1
TASK 1: Hi!
=================================================
 -- c.a.O.S --
   ...
   MCU : STM 32 F:H7xx T:753
=================================================
   System Clock: 480000000 Hz
   ...
   Core Temperature 26 Celsius
   stdout UART3 Baudrate: 115200 bps
=================================================
TASK 1: Hi!
TASK 2: hello!
...
```

A run of 5000 ms takes about 20 s on a desktop PC. The first 3 s of simulated time are
spent in caOS's boot wait for the ST-LINK serial port, which the real board needs.

At the end `run.sh` also lists the **unmapped accesses**: addresses the firmware
touched where the simulated board has nothing. Each one is either a peripheral the
model still lacks, or a firmware bug; on the seven boards, example 2 has none.

Each run leaves two files in `simu/out/`:
- `<elf name>.uart`: the console output;
- `<elf name>.log`: the Renode log, which ends with where the CPU stopped (`cpu PC`, the
  function name, the number of instructions executed). Look there when the console stays empty.

Example 1 is the "no scheduler" hello world: it prints its line and `No current TASK!!`
in a loop, as it does on the board.

### Interactive mode: your terminal on the board's console

```bash
simu/run.sh -i h753 elfs/H753/h753_example_68.elf
```

The console UART becomes a virtual serial port, `/tmp/caos_<board>`, and a terminal
opens on it right in your shell: you see what the firmware prints and you type CLI
commands (`help`, `top`, `fault stack` ...), as on the ST-LINK port of a real board.
The terminal is **picocom** if installed (`sudo apt install picocom`, leave it with
Ctrl-A Ctrl-X), otherwise the small `simu/term.py` that comes with caOS (leave it with
Ctrl-]). Leaving the terminal stops the simulation.

**The Renode monitor** listens on a TCP port, printed at the start
(`>>> Renode monitor on localhost:1234`, or the next free port; `SIMU_MONITOR_PORT`
chooses where to start). From a
second terminal, `telnet localhost 1234` gives the Renode prompt on the running board:

```
pause                                  # freeze the simulated board
cpu PC                                 # where the CPU is
sysbus FindSymbolAt `cpu PC`           # ... as a function name
cpu GetRegistersValues                 # every core register
sysbus ReadDoubleWord 0xE000E010       # any address: memory, SysTick, peripherals
start                                  # let it run again
help                                   # everything else
```

Leave telnet with Ctrl-] and then `quit` (the telnet prompt's, not Renode's: `quit` at
the Renode prompt ends the simulation).

While it runs, other programs can open the same port, e.g.
`CAOS_TTY=/tmp/caos_h753 test/caos_shell`. With **minicom**, switch off *Hardware Flow
Control* (Ctrl-A O, Serial port setup, F): it is on by default, and on a virtual port it
blocks everything you type.

### Typing commands on the console (headless)

A fourth argument types CLI commands after the first period, separated by `;`. Each one
is followed by Enter and by one more second of simulated time (`SIMU_CMD_MS` changes it).
For example, on example 68 (the fault test bench), overflow a task stack into the
MPU guard:

```bash
simu/run.sh u575 elfs/U575/u575_example_68.elf 4000 "help;fault stack"
```

```
[MPU FAULT]
  DACCVIOL: data access violation
  ...
  >> HIT LOWER CANARY @ 0x200B7000
  ...
  Task: f_stack [4]
  Mode: RECOVER (task removed)
```

The task that overflowed is removed, and the rest of the system keeps running.

### Network: Ethernet to your PC (`-e`)

On `f439`, `f767` and `h753`, `-e` connects the board's Ethernet to your PC. The
simulated board gets a LAN8742 PHY with the link up, and a virtual cable to a Linux TAP
interface, `caos-tap0`. On that interface the PC is `192.168.10.1` and a DHCP server
hands out `192.168.10.50-99`, so the network examples work as on a real board:

```bash
simu/run.sh -i -e f767 elfs/F767/f767_example_35.elf
#   DHCP BOUND  IP=192.168.10.65 ...
ping 192.168.10.65                 # from another terminal
```

The TAP needs root once, to be created. If the package's `tools/bench_net.sh` is
there, `run.sh` runs `sudo tools/bench_net.sh tap up` by itself; otherwise it prints
the commands (`ip tuntap add ... user $USER`, the address, a `dnsmasq` line). The TAP
belongs to you, so Renode opens it without root. `sudo tools/bench_net.sh tap down`
removes it. The subnet is the one the real boards use: run the simulation and a real
board on the bench at different times.

DHCP is slow in simulated time: the server checks the address with a ping before
offering it. Give a headless run 40 s or more (`... 40000`).

### Jumpers (`-w`)

Some examples want wires between pins, and have a `wire` command that checks them.
With `-w`, `run.sh` loads `simu/wires/<board>_ex<NN>.repl`, the same wires written as
GPIO connections:

```bash
simu/run.sh -w f767 elfs/F767/f767_example_29.elf 3000 "help;wire"
#   WIRE RESULT : OK  (3/3)
```

These are plain GPIO wires: they make the `wire` check pass. The data itself crosses
only where the simulation models both ends: on F439, F446 and F767, SPI1 and SPI3 are
wired as master and slave, so example 29 runs as on the bench (`EX29 IRQ: frame N OK`).

## 5. Boards

| Argument | Board | Console |
|---|---|---|
| `h753` | NUCLEO-H753ZI | USART3 |
| `f767` | NUCLEO-F767ZI | USART3 |
| `f439` | NUCLEO-F439ZI | USART3 |
| `f446` | NUCLEO-F446ZE | USART3 |
| `u575` | NUCLEO-U575ZI-Q | USART1 |
| `h563` | NUCLEO-H563ZI | USART3 |
| `h773` | NUCLEO-H7S3L8 | USART3 |

The U575, the H563 and the H7S3 run on models written for caOS (Renode has no STM32U5,
STM32H5 or STM32H7S). On the H7S3 the XSPI2 flash and the trampoline are not
simulated: the image is loaded at 0x70000000 and starts from its own vector table.
All seven boards of the FREE edition are available.

## 6. Automatic tests

```bash
simu/test.sh
```

Runs the Robot Framework suites in `simu/tests/` with `renode-test`. Each test boots an
example and waits for the expected console lines; a test fails if the line does not
appear in time or if a fault is printed. Boards whose `.elf` files you have not built
are skipped. The HTML report goes to `simu/out/robot/log.html`.

- `boot.robot`: every board boots (examples 1, 2, 68).
- `wires.robot`: every example with bench jumpers answers `wire` with OK on every
  board (generated by `simu/gen_wires.py`, which also writes `simu/wires/`).
- `peripherals.robot`: example 29 (SPI master to slave, with the jumpers of `-w`),
  `reset` and `reboot` (watchdog) bring the board back, example 35 takes a DHCP
  lease and answers a ping from the PC. The Ethernet tests are tagged `eth` and are
  skipped unless `caos-tap0` is up (`sudo tools/bench_net.sh tap up`).

One suite or one tag: `simu/test.sh --include eth "$PWD/simu/tests/peripherals.robot"`.

## Limits

In short: time is not real time (cycle counts are not representative), the clock
tree is fixed, a few fault details differ, and many peripherals are only stubs or
missing: USB is not connected, Ethernet only on F439/F767/H753 with `-e`.
**`LIMITS.md`** has the full list, and explains how the simulation is built and how
to extend it (clock tree, USB). What we found while bringing Renode up is in `CHANGES.md`.

## Troubleshooting

- **No output at all**: read the end of `simu/out/<elf>.log`. `cpu PC` and the function
  name show where the firmware stopped.
- **`run.sh` prints nothing for a long time**: the headless mode prints only at the
  end, after about 20 s for 5000 ms. If Renode uses no CPU at all (`top`), it is stuck:
  stop it with Ctrl-C and look at `simu/out/<elf>.log`.
- **Typing in the console does nothing** (with your own terminal program on
  `/tmp/caos_<board>`): hardware flow control is on (minicom's default). Switch it
  off, or use `run.sh -i`, which opens picocom or `term.py` for you.
- **`renode: command not found`**: the portable folder is not in `PATH` (section 2a).
- **`dotnet not found`**: you installed the `.deb`/`.rpm` package without the .NET
  runtime (section 2b), or switch to the portable package.
- **`gen_platforms: no .../platforms/cpus`**: Renode is not where the scripts look;
  set `RENODE_ROOT`.
- **`No module named 'robot'`**: the venv for `renode-test` is missing (end of section 2).
